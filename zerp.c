/*
 * WynlandOS - Zerp compositor (v0)
 * ============================================================
 * A real Ring-3 process, not kernel code -- the whole point of this
 * project. Opens /dev/fb0 (the same proven path test_raw_fb.elf uses,
 * see Phase 2's fb_active_phys_addr() writeup) plus /dev/input/mice and
 * /dev/input/kbd, and is the ONLY process that should touch them
 * (documented convention, not kernel-enforced, same as Phase 1/2).
 *
 * Spawns a small hardcoded set of client demos, each with a fresh pipe
 * pair (control messages, see zerp_protocol.h) and SHM segment (pixel
 * data, see kernel/syscall.c's SYS_shm_create/0xFFFFFFFD SYS_mmap
 * branch), tiles them (simple dwindle-style recursive split, recomputed
 * from scratch on membership change), and runs a non-blocking main loop:
 * poll client damage -> blit their SHM buffer into /dev/fb0 -> poll
 * mouse/keyboard -> route input to whichever client's tile the cursor is
 * over -> one coalesced SYS_fb_flush per iteration if anything changed.
 *
 * Freestanding, no libc -- same style as spawner.c/shm_test_*.c. Build:
 *   x86_64-linux-musl-gcc -nostdlib -static -no-pie -fno-pie \
 *     -mcmodel=large -O2 -Wl,-Ttext-segment=0x340000000000 \
 *     -o zerp.elf zerp.c
 */
#include "zerp_syscalls.h"
#include "zerp_protocol.h"

#define MAX_CLIENTS 16
#define BORDER_COLOR 0xFF444444

typedef struct {
    int c2s_read_fd;
    int s2c_write_fd;
    uint32_t *shm;
    uint32_t tile_x, tile_y, tile_w, tile_h;
    int alive;
    long pid;
} ClientSlot;

static ClientSlot g_clients[MAX_CLIENTS];
static int g_client_count = 0;

static uint32_t g_screen_w, g_screen_h;
static uint32_t *g_fb;
static uint32_t g_fb_pitch_pixels;
static uint32_t g_shm_bytes; /* g_screen_w * g_screen_h * 4, set once at boot;
                                reused by every dynamic (runtime) spawn too --
                                a client's tile can never exceed the real
                                screen, so this stays a safe upper bound. */

static int spawn_client(const char *path, uint32_t shm_bytes) {
    if (g_client_count >= MAX_CLIENTS) return -1;

    int c2s[2], s2c[2];
    if (zpipe(c2s) < 0) return -1;
    if (zpipe(s2c) < 0) { zclose(c2s[0]); zclose(c2s[1]); return -1; }

    long shm_fd = zshm_create(shm_bytes);
    if (shm_fd < 0) {
        zclose(c2s[0]); zclose(c2s[1]); zclose(s2c[0]); zclose(s2c[1]);
        return -1;
    }

    /* Server keeps c2s[0] (read) and s2c[1] (write) long-term -- mark
       CLOEXEC so they never leak into a LATER spawn_client() call
       (process_spawn()'s fd inheritance copies every non-CLOEXEC fd the
       caller currently has open, not just ones meant for this child). */
    zfcntl(c2s[0], F_SETFD, FD_CLOEXEC);
    zfcntl(s2c[1], F_SETFD, FD_CLOEXEC);
    /* c2s[1], s2c[0], shm_fd stay non-CLOEXEC (default) -- inherited by
       this one spawn only. */

    char c2s_w_str[16], s2c_r_str[16], shm_str[16];
    znum_to_str(c2s[1], c2s_w_str);
    znum_to_str(s2c[0], s2c_r_str);
    znum_to_str(shm_fd, shm_str);
    const char *argv[5];
    argv[0] = path;
    argv[1] = c2s_w_str;
    argv[2] = s2c_r_str;
    argv[3] = shm_str;
    argv[4] = (const char *)0;

    long pid = zspawn_argv(path, argv);

    /* Standard pipe+fork idiom: close OUR copies of the child-facing fds
       right after spawning -- we don't need them, and if left open
       (non-CLOEXEC) they'd leak into every subsequent spawn_client()
       call's child too. */
    zclose(c2s[1]);
    zclose(s2c[0]);

    if (pid < 0) {
        zclose(c2s[0]);
        zclose(s2c[1]);
        return -1;
    }

    int idx = g_client_count;
    g_clients[idx].c2s_read_fd  = c2s[0];
    g_clients[idx].s2c_write_fd = s2c[1];
    g_clients[idx].shm = (uint32_t *)zmmap_shm(shm_fd, shm_bytes);
    g_clients[idx].tile_x = g_clients[idx].tile_y = 0;
    g_clients[idx].tile_w = g_clients[idx].tile_h = 0;
    g_clients[idx].alive = 1;
    g_clients[idx].pid = pid;
    g_client_count++;
    return idx;
}

/* Removes every slot with alive==0, compacting the survivors down so
   MAX_CLIENTS bounds *concurrently open* windows, not the total number
   ever opened in a session (which the previous append-only, never-shrink
   g_clients[] effectively did). */
static void compact_clients(void) {
    int w = 0;
    for (int i = 0; i < g_client_count; i++) {
        if (g_clients[i].alive) {
            if (w != i) g_clients[w] = g_clients[i];
            w++;
        }
    }
    g_client_count = w;
}

/* Dwindle-style tiling: recompute every live client's rect from scratch
   (not incremental merge/split bookkeeping -- only runs on membership
   change, cheap enough). Rect 0 gets the full screen; each subsequent
   client splits the currently-largest rect in half along its longer
   axis. Sends each client its new ZERP_MSG_TILE_RECT afterward. */
static void retile(void) {
    int n_alive = 0;
    int order[MAX_CLIENTS];
    for (int i = 0; i < g_client_count; i++) {
        if (g_clients[i].alive) order[n_alive++] = i;
    }
    if (n_alive == 0) return;

    uint32_t rx[MAX_CLIENTS], ry[MAX_CLIENTS], rw[MAX_CLIENTS], rh[MAX_CLIENTS];
    rx[0] = 0; ry[0] = 0; rw[0] = g_screen_w; rh[0] = g_screen_h;
    int n = 1;
    while (n < n_alive) {
        int best = 0;
        uint32_t best_area = rw[0] * rh[0];
        for (int i = 1; i < n; i++) {
            uint32_t area = rw[i] * rh[i];
            if (area > best_area) { best_area = area; best = i; }
        }
        if (rw[best] >= rh[best]) {
            uint32_t half = rw[best] / 2;
            rx[n] = rx[best] + half; ry[n] = ry[best];
            rw[n] = rw[best] - half; rh[n] = rh[best];
            rw[best] = half;
        } else {
            uint32_t half = rh[best] / 2;
            rx[n] = rx[best]; ry[n] = ry[best] + half;
            rw[n] = rw[best]; rh[n] = rh[best] - half;
            rh[best] = half;
        }
        n++;
    }

    for (int i = 0; i < n_alive; i++) {
        ClientSlot *c = &g_clients[order[i]];
        c->tile_x = rx[i]; c->tile_y = ry[i];
        c->tile_w = rw[i]; c->tile_h = rh[i];
        ZerpMsg msg;
        msg.type = ZERP_MSG_TILE_RECT;
        msg.x = rx[i]; msg.y = ry[i]; msg.w = rw[i]; msg.h = rh[i];
        zwrite(c->s2c_write_fd, &msg, sizeof(msg));
    }
}

static void blit_client(int idx) {
    ClientSlot *c = &g_clients[idx];
    if (!c->shm || c->tile_w == 0 || c->tile_h == 0) return;

    for (uint32_t row = 0; row < c->tile_h; row++) {
        uint32_t *src = &c->shm[row * c->tile_w];
        uint32_t *dst = &g_fb[(c->tile_y + row) * g_fb_pitch_pixels + c->tile_x];
        for (uint32_t col = 0; col < c->tile_w; col++) dst[col] = src[col];
    }
    for (uint32_t x = c->tile_x; x < c->tile_x + c->tile_w; x++) {
        g_fb[c->tile_y * g_fb_pitch_pixels + x] = BORDER_COLOR;
        g_fb[(c->tile_y + c->tile_h - 1) * g_fb_pitch_pixels + x] = BORDER_COLOR;
    }
    for (uint32_t y = c->tile_y; y < c->tile_y + c->tile_h; y++) {
        g_fb[y * g_fb_pitch_pixels + c->tile_x] = BORDER_COLOR;
        g_fb[y * g_fb_pitch_pixels + c->tile_x + c->tile_w - 1] = BORDER_COLOR;
    }
}

int zerp_main(int argc, char **argv) {
    (void)argc; (void)argv;
    zwrite(1, "[zerp] start\n", 13);

    long fb_fd = zopen("/dev/fb0", O_RDWR);
    if (fb_fd < 0) { zwrite(1, "[zerp] FATAL: open /dev/fb0 failed\n", 36); zexit(1); }

    struct zerp_fb_var_screeninfo vinfo;
    struct zerp_fb_fix_screeninfo finfo;
    zioctl((int)fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    zioctl((int)fb_fd, FBIOGET_FSCREENINFO, &finfo);
    g_screen_w = vinfo.xres;
    g_screen_h = vinfo.yres;
    g_fb_pitch_pixels = finfo.line_length / 4;

    long fb_bytes = (long)finfo.line_length * g_screen_h;
    g_fb = (uint32_t *)zsys6(SYS_mmap, 0, fb_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, (int)fb_fd, 0);
    if ((long)g_fb <= 0) { zwrite(1, "[zerp] FATAL: mmap /dev/fb0 failed\n", 36); zexit(1); }

    long mice_fd = zopen("/dev/input/mice", O_RDONLY);
    long kbd_fd  = zopen("/dev/input/kbd", O_RDONLY);

    g_shm_bytes = g_screen_w * g_screen_h * 4;
    spawn_client("/zerp_files.elf", g_shm_bytes);
    spawn_client("/zerp_term.elf", g_shm_bytes);

    retile();
    zwrite(1, "[zerp] clients spawned and tiled\n", 34);

    int32_t cursor_x = (int32_t)(g_screen_w / 2);
    int32_t cursor_y = (int32_t)(g_screen_h / 2);
    uint8_t mouse_buf[3];
    int mouse_cycle = 0;
    int focused = 0;

    for (;;) {
        /* -- mouse: decode standard 3-byte PS/2 packets, same sign/
           inversion convention as drivers/input/mouse.c's own IRQ
           handler (byte0=buttons+sign bits, byte1=dx, byte2=dy,
           Y inverted) -- replicated here since Zerp reads the raw
           /dev/input/mice byte stream itself rather than the kernel's
           own cursor state. */
        uint8_t mb;
        while (zread((int)mice_fd, &mb, 1) == 1) {
            mouse_buf[mouse_cycle++] = mb;
            if (mouse_cycle < 3) continue;
            mouse_cycle = 0;

            uint8_t buttons = mouse_buf[0];
            int32_t rel_x = (int32_t)mouse_buf[1];
            int32_t rel_y = (int32_t)mouse_buf[2];
            if (buttons & 0x10) rel_x |= ~0xFF;
            if (buttons & 0x20) rel_y |= ~0xFF;

            cursor_x += rel_x;
            cursor_y -= rel_y;
            if (cursor_x < 0) cursor_x = 0;
            if (cursor_x >= (int32_t)g_screen_w) cursor_x = (int32_t)g_screen_w - 1;
            if (cursor_y < 0) cursor_y = 0;
            if (cursor_y >= (int32_t)g_screen_h) cursor_y = (int32_t)g_screen_h - 1;

            for (int i = 0; i < g_client_count; i++) {
                ClientSlot *c = &g_clients[i];
                if (!c->alive) continue;
                if ((uint32_t)cursor_x >= c->tile_x && (uint32_t)cursor_x < c->tile_x + c->tile_w &&
                    (uint32_t)cursor_y >= c->tile_y && (uint32_t)cursor_y < c->tile_y + c->tile_h) {
                    focused = i;
                    break;
                }
            }

            if (g_client_count > 0 && g_clients[focused].alive) {
                ZerpMsg mmsg;
                mmsg.type = ZERP_MSG_INPUT_MOUSE;
                mmsg.x = (uint32_t)cursor_x - g_clients[focused].tile_x;
                mmsg.y = (uint32_t)cursor_y - g_clients[focused].tile_y;
                mmsg.w = buttons & 0x07;
                mmsg.h = 0;
                zwrite(g_clients[focused].s2c_write_fd, &mmsg, sizeof(mmsg));
            }
        }

        /* -- keyboard: forward raw scancodes to the focused client only. */
        uint8_t kb;
        while (g_client_count > 0 && g_clients[focused].alive && zread((int)kbd_fd, &kb, 1) == 1) {
            ZerpMsg kmsg;
            kmsg.type = ZERP_MSG_INPUT_KEY;
            kmsg.x = kb; kmsg.y = 0; kmsg.w = 0; kmsg.h = 0;
            zwrite(g_clients[focused].s2c_write_fd, &kmsg, sizeof(kmsg));
        }

        /* -- client control messages: DAMAGE/CLOSE/SPAWN. Every message
           starts with a 4-byte type field; read that first, then read the
           REST of the message at a size depending on what the type turned
           out to be (ZerpMsg's 16 remaining bytes, or ZerpSpawnMsg's 252
           path bytes) -- see zerp_protocol.h's ZerpSpawnMsg comment for
           why this split-read is safe here (sender always writes one
           struct in a single zwrite(), never a partial one). */
        int any_damage = 0;
        int need_retile = 0;
        uint32_t union_x = g_screen_w, union_y = g_screen_h, union_x2 = 0, union_y2 = 0;
        for (int i = 0; i < g_client_count; i++) {
            ClientSlot *c = &g_clients[i];
            if (!c->alive) continue;
            uint32_t type;
            while (zread(c->c2s_read_fd, &type, sizeof(type)) == (long)sizeof(type)) {
                if (type == ZERP_MSG_SPAWN) {
                    char path[252];
                    zread(c->c2s_read_fd, path, sizeof(path));
                    path[sizeof(path) - 1] = '\0';
                    zwrite(1, "[zerp] SPAWN request for: ", 27);
                    zwrite(1, path, zstrlen(path));
                    zwrite(1, "\n", 1);
                    int new_idx = spawn_client(path, g_shm_bytes);
                    zwrite(1, "[zerp] spawn_client returned idx=", 34);
                    { char nb[8]; znum_to_str(new_idx, nb); zwrite(1, nb, zstrlen(nb)); }
                    zwrite(1, "\n", 1);
                    if (new_idx >= 0) {
                        need_retile = 1;
                        /* Focus-follows-new-window: a launcher (rofi) or any
                           other client that spawns something expects the new
                           window to be immediately ready for keyboard input,
                           same as any real WM. */
                        focused = new_idx;
                    }
                } else {
                    uint32_t rest[4];
                    zread(c->c2s_read_fd, rest, sizeof(rest));
                    if (type == ZERP_MSG_DAMAGE) {
                        blit_client(i);
                        any_damage = 1;
                        if (c->tile_x < union_x) union_x = c->tile_x;
                        if (c->tile_y < union_y) union_y = c->tile_y;
                        if (c->tile_x + c->tile_w > union_x2) union_x2 = c->tile_x + c->tile_w;
                        if (c->tile_y + c->tile_h > union_y2) union_y2 = c->tile_y + c->tile_h;
                    } else if (type == ZERP_MSG_CLOSE) {
                        c->alive = 0;
                        need_retile = 1;
                    }
                }
            }
        }

        if (need_retile) {
            compact_clients();
            if (focused >= g_client_count) focused = g_client_count > 0 ? g_client_count - 1 : 0;
            retile();
        }

        if (any_damage) {
            zfb_flush((int)union_x, (int)union_y, (int)(union_x2 - union_x), (int)(union_y2 - union_y));
        }

        zyield();
    }
}

#include "zerp_entry.h"
