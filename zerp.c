/*
 * WynlandOS - Zerp compositor (v0)
 * ============================================================
 * A real Ring-3 process, not kernel code -- the whole point of this
 * project. Opens /dev/fb0 (the same proven path test_raw_fb.elf uses,
 * see Phase 2's fb_active_phys_addr() writeup) plus /dev/input/kbd, and
 * is the ONLY process that should touch them (documented convention, not
 * kernel-enforced, same as Phase 1/2). Pointer input comes from the
 * kernel's SYS_mouse_events stream, which IS exclusive to one process.
 *
 * Spawns a small hardcoded set of client demos, each with a fresh pipe
 * pair (control messages, see zerp_protocol.h) and SHM segment (pixel
 * data, see kernel/syscall.c's SYS_shm_create/0xFFFFFFFD SYS_mmap
 * branch), tiles them (simple dwindle-style recursive split, recomputed
 * from scratch on membership change), and runs a frame-paced main loop:
 * poll mouse/keyboard -> route input to whichever client's tile the cursor
 * is over -> collect client damage rects -> at most ZERP_HZ times a second
 * (120) blit ONLY the damaged sub-rects of their SHM buffers into
 * /dev/fb0 and issue one SYS_fb_flush for the frame's damage union ->
 * sleep 1ms. The pointer is not drawn here at all: the kernel drives the
 * virtio-gpu hardware cursor plane straight from the mouse IRQ.
 *
 * Freestanding, no libc -- same style as spawner.c/shm_test_*.c. Build:
 *   x86_64-linux-musl-gcc -nostdlib -static -no-pie -fno-pie \
 *     -mcmodel=large -O2 -Wl,-Ttext-segment=0x340000000000 \
 *     -o zerp.elf zerp.c
 */
#include "zerp_syscalls.h"
#include "zerp_protocol.h"
#include "zerp_png.h"

#define MAX_CLIENTS 16
#define BORDER_COLOR      0xFF3A3A44
#define FOCUS_BORDER      0xFF3B82F6
#define UNFOCUS_BORDER    0xFF26262C
#define BAR_BG            0xFF17171B
#define BAR_ACCENT        0xFF3B82F6
#define OUTER_STROKE      0xFF0E0E12
#define TILE_GAP          6
#define TITLE_H           20

#define ZERP_OUTBOX 128

typedef struct {
    int c2s_read_fd;
    int s2c_write_fd;
    uint32_t *shm;
    uint32_t tile_x, tile_y, tile_w, tile_h;
    int alive;
    long pid;
    /* damage accumulated since the last present, content-relative,
       x2/y2 exclusive; blitted once per frame (not once per message) */
    int dmg;
    uint32_t dmg_x1, dmg_y1, dmg_x2, dmg_y2;
    /* c2s is non-blocking: bytes are accumulated here and only WHOLE
       messages are parsed, so a message that isn't all there yet waits
       for the next iteration instead of desyncing the stream. */
    uint8_t rx[512];
    uint32_t rx_len;
    /* s2c is non-blocking too: messages the pipe can't take right now
       wait here instead of being dropped (a lost button-up or key-up
       means a stuck button/key). Pure pointer motion is coalesced. */
    ZerpMsg out[ZERP_OUTBOX];
    uint32_t out_head, out_count;
    int out_tail_motion; /* newest queued message is pure motion */
    /* Latest TILE_RECT that didn't fit a full, un-evictable outbox; sent
       as soon as the outbox drains. */
    ZerpMsg tile_pending;
    int has_tile_pending;
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

/* focused-client mirror for the blit/chrome code (main loop owns `focused`) */
static int g_focused_client = 0;

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
    /* The main loop DRAINS every client pipe each iteration: an empty one
       must return -EAGAIN, not park the compositor until that client
       happens to speak (pipes block by default since Phase 22d). */
    zfcntl(c2s[0], F_SETFL, O_NONBLOCK);
    /* Likewise a client that stops reading (busy/hung) must not freeze the
       compositor on a full s2c pipe: its input is dropped instead, the
       same thing a real compositor does to an unresponsive client. */
    zfcntl(s2c[1], F_SETFL, O_NONBLOCK);
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
    g_clients[idx].dmg = 0;
    g_clients[idx].rx_len = 0;
    g_clients[idx].out_head = g_clients[idx].out_count = 0;
    g_clients[idx].out_tail_motion = 0;
    g_clients[idx].has_tile_pending = 0;
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

/* Push as much of the outbox into the s2c pipe as it takes. Each ZerpMsg
   goes whole or not at all (kernel pipes are PIPE_BUF-atomic). */
static void client_flush_out(ClientSlot *c) {
    while (c->out_count > 0) {
        if (zwrite(c->s2c_write_fd, &c->out[c->out_head], sizeof(ZerpMsg)) != (long)sizeof(ZerpMsg))
            return;
        c->out_head = (c->out_head + 1) % ZERP_OUTBOX;
        c->out_count--;
    }
    if (c->has_tile_pending &&
        zwrite(c->s2c_write_fd, &c->tile_pending, sizeof(ZerpMsg)) == (long)sizeof(ZerpMsg))
        c->has_tile_pending = 0;
}

static ZerpMsg *outbox_at(ClientSlot *c, uint32_t k) {
    return &c->out[(c->out_head + k) % ZERP_OUTBOX];
}

static void outbox_remove(ClientSlot *c, uint32_t k) {
    for (; k + 1 < c->out_count; k++) *outbox_at(c, k) = *outbox_at(c, k + 1);
    c->out_count--;
}

/* Outbox full (the client hasn't read in a long time): free one slot,
   losing as little as possible. A mouse message may go only if the next
   queued mouse message has the SAME button mask -- then only a position
   is lost, never a press or release (dropping a press followed by its
   release would lose the click). Otherwise a TILE_RECT superseded by a
   newer one. Key events and the newest TILE_RECT are never evicted; if
   nothing qualifies, returns 0. */
static int outbox_evict(ClientSlot *c) {
    for (uint32_t k = 0; k < c->out_count; k++) {
        ZerpMsg *a = outbox_at(c, k);
        if (a->type != ZERP_MSG_INPUT_MOUSE) continue;
        for (uint32_t l = k + 1; l < c->out_count; l++) {
            ZerpMsg *b = outbox_at(c, l);
            if (b->type != ZERP_MSG_INPUT_MOUSE) continue;
            if (b->w == a->w) { outbox_remove(c, k); return 1; }
            break;
        }
    }
    for (uint32_t k = 0; k < c->out_count; k++) {
        if (outbox_at(c, k)->type != ZERP_MSG_TILE_RECT) continue;
        for (uint32_t l = k + 1; l < c->out_count; l++) {
            if (outbox_at(c, l)->type == ZERP_MSG_TILE_RECT) { outbox_remove(c, k); return 1; }
        }
        break; /* the oldest TILE_RECT has no successor: none do */
    }
    return 0;
}

/* `motion` = pure pointer motion (no button change). Replacing a
   still-queued pure-motion message loses nothing -- only the latest
   position matters -- but a button change always keeps its own slot, so
   a click is delivered at the position it happened at. */
static void client_send_ex(ClientSlot *c, const ZerpMsg *m, int motion) {
    if (motion && c->out_count > 0 && c->out_tail_motion) {
        *outbox_at(c, c->out_count - 1) = *m;
        client_flush_out(c);
        return;
    }
    if (m->type == ZERP_MSG_TILE_RECT && c->has_tile_pending) {
        c->tile_pending = *m; /* keep its place after the queued input */
        client_flush_out(c);
        return;
    }
    if (c->out_count == ZERP_OUTBOX && !outbox_evict(c)) {
        /* Only key events queued: the client is wedged. A TILE_RECT
           waits in its own slot; anything else is dropped (key events
           go through client_send_key(), which never gets here). */
        if (m->type == ZERP_MSG_TILE_RECT) { c->tile_pending = *m; c->has_tile_pending = 1; }
        return;
    }
    *outbox_at(c, c->out_count) = *m;
    c->out_count++;
    c->out_tail_motion = motion;
    client_flush_out(c);
}

static void client_send(ClientSlot *c, const ZerpMsg *m) { client_send_ex(c, m, 0); }

/* One scancode, E0-prefixed or not, queued all-or-nothing so the prefix
   can never be separated from its code. If a wedged client's outbox has
   no room even after eviction, the key is dropped as a unit -- which can
   still split a make from its break (a stuck key in that client). That is
   accepted: it takes 128 queued key events the client never read. */
static void client_send_key(ClientSlot *c, int e0, uint8_t sc) {
    uint32_t need = e0 ? 2 : 1;
    while (ZERP_OUTBOX - c->out_count < need) {
        if (!outbox_evict(c)) return;
    }
    ZerpMsg k = { ZERP_MSG_INPUT_KEY, 0xE0, 0, 0, 0 };
    if (e0) { *outbox_at(c, c->out_count) = k; c->out_count++; }
    k.x = sc;
    *outbox_at(c, c->out_count) = k; c->out_count++;
    c->out_tail_motion = 0;
    client_flush_out(c);
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
        uint32_t ccx = rx[i] + TILE_GAP;
        uint32_t ccy = ry[i] + TILE_GAP + TITLE_H;
        uint32_t ccw = rw[i] - 2 * TILE_GAP;
        uint32_t cch = rh[i] - 2 * TILE_GAP - TITLE_H;
        msg.x = ccx; msg.y = ccy;
        msg.w = (ccw > 8) ? ccw : 8;
        msg.h = (cch > 8) ? cch : 8;
        client_send(c, &msg);
    }
}

/* Content rect = tile minus outer gap and titlebar strip. Clients receive
   THIS rect in ZERP_MSG_TILE_RECT and render into it; the compositor owns
   everything inside the tile outside it (titlebar + borders). */
static void content_rect(ClientSlot *c, uint32_t *cx, uint32_t *cy,
                         uint32_t *cw, uint32_t *ch) {
    *cx = c->tile_x + TILE_GAP;
    *cy = c->tile_y + TILE_GAP + TITLE_H;
    uint32_t w = c->tile_w - 2 * TILE_GAP;
    uint32_t h = c->tile_h - 2 * TILE_GAP - TITLE_H;
    *cw = (w > 8) ? w : 8;
    *ch = (h > 8) ? h : 8;
}

/* ---- Screen damage: everything written to g_fb since the last present ---- */
static uint32_t g_scr_x1, g_scr_y1, g_scr_x2, g_scr_y2; /* x2/y2 exclusive */
static int g_scr_damaged = 0;

static void screen_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return;
    uint32_t x2 = x + w, y2 = y + h;
    if (x2 > g_screen_w) x2 = g_screen_w;
    if (y2 > g_screen_h) y2 = g_screen_h;
    if (x >= x2 || y >= y2) return;
    if (!g_scr_damaged) {
        g_scr_x1 = x; g_scr_y1 = y; g_scr_x2 = x2; g_scr_y2 = y2;
        g_scr_damaged = 1;
        return;
    }
    if (x < g_scr_x1) g_scr_x1 = x;
    if (y < g_scr_y1) g_scr_y1 = y;
    if (x2 > g_scr_x2) g_scr_x2 = x2;
    if (y2 > g_scr_y2) g_scr_y2 = y2;
}

/* One transfer+flush of the whole frame's damage union (a single batched
   virtio-gpu round trip in the kernel). */
static void screen_present(void) {
    if (!g_scr_damaged) return;
    zfb_flush((int)g_scr_x1, (int)g_scr_y1,
              (int)(g_scr_x2 - g_scr_x1), (int)(g_scr_y2 - g_scr_y1));
    g_scr_damaged = 0;
}

/* ---- Per-client pending damage, in the client's own content coords ---- */
static void client_add_damage(ClientSlot *c, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return;
    uint32_t x2 = x + w, y2 = y + h;
    if (x2 < x) x2 = 0xFFFFFFFFu; /* overflow from a bogus rect: clip later */
    if (y2 < y) y2 = 0xFFFFFFFFu;
    if (!c->dmg) {
        c->dmg_x1 = x; c->dmg_y1 = y; c->dmg_x2 = x2; c->dmg_y2 = y2;
        c->dmg = 1;
        return;
    }
    if (x < c->dmg_x1) c->dmg_x1 = x;
    if (y < c->dmg_y1) c->dmg_y1 = y;
    if (x2 > c->dmg_x2) c->dmg_x2 = x2;
    if (y2 > c->dmg_y2) c->dmg_y2 = y2;
}

/* Wallpaper (or whatever was on screen at startup) for gap/stale areas. */
static uint32_t *g_wall = 0;

static void paint_background(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (x >= g_screen_w || y >= g_screen_h) return;
    if (x + w > g_screen_w) w = g_screen_w - x;
    if (y + h > g_screen_h) h = g_screen_h - y;
    for (uint32_t row = y; row < y + h; row++) {
        uint32_t *dst = &g_fb[row * g_fb_pitch_pixels + x];
        if (!g_wall) break; /* no copy of the background: leave it be */
        zcopy32(dst, &g_wall[row * g_screen_w + x], w);
    }
    screen_damage(x, y, w, h);
}

static void fill_fb_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (x >= g_screen_w || y >= g_screen_h) return;
    if (x + w > g_screen_w) w = g_screen_w - x;
    if (y + h > g_screen_h) h = g_screen_h - y;
    for (uint32_t row = y; row < y + h; row++) {
        zfill32(&g_fb[row * g_fb_pitch_pixels + x], color, w);
    }
}

/* Titlebar, borders and outer stroke -- only on retile / focus change,
   never per content frame. */
static void draw_chrome(int idx) {
    ClientSlot *c = &g_clients[idx];
    if (c->tile_w == 0 || c->tile_h == 0) return;

    uint32_t cx, cy, cw, ch;
    content_rect(c, &cx, &cy, &cw, &ch);
    int focused = (idx == g_focused_client);

    /* titlebar strip + accent underline (bright when focused) */
    fill_fb_rect(cx, c->tile_y + TILE_GAP, cw, TITLE_H, BAR_BG);
    fill_fb_rect(cx, c->tile_y + TILE_GAP + TITLE_H - 2, cw, 1,
                 focused ? BAR_ACCENT : UNFOCUS_BORDER);

    /* content border */
    uint32_t bc = focused ? FOCUS_BORDER : UNFOCUS_BORDER;
    fill_fb_rect(cx - 1, cy - 1, cw + 2, 1, bc);
    fill_fb_rect(cx - 1, cy + ch, cw + 2, 1, bc);
    fill_fb_rect(cx - 1, cy, 1, ch, bc);
    fill_fb_rect(cx + cw, cy, 1, ch, bc);

    /* outer tile stroke (subtle separation against the wallpaper) */
    fill_fb_rect(c->tile_x, c->tile_y, c->tile_w, 1, OUTER_STROKE);
    fill_fb_rect(c->tile_x, c->tile_y + c->tile_h - 1, c->tile_w, 1, OUTER_STROKE);
    fill_fb_rect(c->tile_x, c->tile_y, 1, c->tile_h, OUTER_STROKE);
    fill_fb_rect(c->tile_x + c->tile_w - 1, c->tile_y, 1, c->tile_h, OUTER_STROKE);

    screen_damage(c->tile_x, c->tile_y, c->tile_w, c->tile_h);
}

/* Copy ONLY the damaged part of the client's SHM into the framebuffer. */
static void blit_client_damage(int idx) {
    ClientSlot *c = &g_clients[idx];
    if (!c->dmg) return;
    c->dmg = 0;
    if (!c->shm || c->tile_w == 0 || c->tile_h == 0) return;

    uint32_t cx, cy, cw, ch;
    content_rect(c, &cx, &cy, &cw, &ch);

    uint32_t x1 = c->dmg_x1, y1 = c->dmg_y1;
    uint32_t x2 = c->dmg_x2 < cw ? c->dmg_x2 : cw;
    uint32_t y2 = c->dmg_y2 < ch ? c->dmg_y2 : ch;
    if (cy + y2 > g_screen_h) y2 = g_screen_h - cy;
    if (x1 >= x2 || y1 >= y2) return;

    uint32_t w = x2 - x1;
    for (uint32_t row = y1; row < y2; row++) {
        zcopy32(&g_fb[(cy + row) * g_fb_pitch_pixels + cx + x1],
                &c->shm[row * cw + x1], w);
    }
    screen_damage(cx + x1, cy + y1, w, y2 - y1);
}

/* Full-screen relayout: background everywhere, chrome for every tile,
   content areas blanked until each client re-renders at its new size
   (its SHM still has the OLD stride until then -- blitting it now would
   show sheared garbage). */
static void repaint_layout(void) {
    paint_background(0, 0, g_screen_w, g_screen_h);
    for (int i = 0; i < g_client_count; i++) {
        ClientSlot *c = &g_clients[i];
        if (!c->alive) continue;
        c->dmg = 0;
        uint32_t cx, cy, cw, ch;
        content_rect(c, &cx, &cy, &cw, &ch);
        fill_fb_rect(cx, cy, cw, ch, BAR_BG);
        draw_chrome(i);
    }
}

static void set_focus(int new_focus, int *focused) {
    if (new_focus == *focused) return;
    int old = *focused;
    *focused = new_focus;
    g_focused_client = new_focus;
    if (old >= 0 && old < g_client_count && g_clients[old].alive) draw_chrome(old);
    if (new_focus >= 0 && new_focus < g_client_count && g_clients[new_focus].alive) draw_chrome(new_focus);
}

/* Focus-follows-mouse + forward the pointer to the focused client. */
/* Buttons held as of the last delivered mouse message. While any is held
   the pointer is grabbed by the client it was pressed in (like X11's
   implicit grab): no focus change, and the release goes to that client
   too -- otherwise dragging out of a window left it with a button that
   never came up. */
static uint8_t g_mouse_held = 0;

static void deliver_mouse(int32_t mx, int32_t my, uint8_t buttons, int motion, int *focused) {
    int grabbed = g_mouse_held != 0;
    g_mouse_held = buttons;
    for (int i = 0; !grabbed && i < g_client_count; i++) {
        ClientSlot *c = &g_clients[i];
        if (!c->alive) continue;
        if ((uint32_t)mx >= c->tile_x && (uint32_t)mx < c->tile_x + c->tile_w &&
            (uint32_t)my >= c->tile_y && (uint32_t)my < c->tile_y + c->tile_h) {
            set_focus(i, focused);
            break;
        }
    }
    if (g_client_count > 0 && g_clients[*focused].alive) {
        ClientSlot *c = &g_clients[*focused];
        /* Relative to the CONTENT rect -- the area the client actually
           renders (its SHM buffer), what TILE_RECT told it -- not to the
           tile, whose gap and titlebar belong to the compositor. Clamped
           into it: the pointer can be outside (dragged out under the grab,
           over the titlebar or a gap), and the unsigned wire fields would
           turn "left of it" into a huge x. */
        uint32_t cx, cy, cw, ch;
        content_rect(c, &cx, &cy, &cw, &ch);
        int32_t rx = mx - (int32_t)cx;
        int32_t ry = my - (int32_t)cy;
        if (rx < 0) rx = 0;
        if (ry < 0) ry = 0;
        if (cw > 0 && rx >= (int32_t)cw) rx = (int32_t)cw - 1;
        if (ch > 0 && ry >= (int32_t)ch) ry = (int32_t)ch - 1;
        ZerpMsg mmsg;
        mmsg.type = ZERP_MSG_INPUT_MOUSE;
        mmsg.x = (uint32_t)rx;
        mmsg.y = (uint32_t)ry;
        mmsg.w = buttons;
        mmsg.h = 0;
        client_send_ex(&g_clients[*focused], &mmsg, motion);
    }
}

/* Target refresh: 120 Hz. Damage arriving faster than this is merged
   into one present; the hardware cursor is independent of it. Frame
   deadlines advance by exactly 1000/120 ms (tracked in 1/ZERP_HZ ms
   units) instead of "8ms after the last present", so loop wake-up jitter
   doesn't drift the rate below target. */
#define ZERP_HZ 120

/* Set to 1 to print presents/damage-messages per second to the serial log. */
#ifndef ZERP_STATS
#define ZERP_STATS 0
#endif

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

    long kbd_fd  = zopen("/dev/input/kbd", O_RDONLY);

    /* Screen-sized wallpaper buffer (an anonymous SHM segment is this
       freestanding binary's only allocator). Starts as a snapshot of the
       current screen -- the bootloader/kernel gradient -- so retiles can
       restore gap areas instead of leaving stale window pixels there. */
    {
        long wall_fd = zshm_create((long)g_screen_w * g_screen_h * 4);
        if (wall_fd >= 0) {
            uint32_t *w = (uint32_t *)zmmap_shm(wall_fd, (long)g_screen_w * g_screen_h * 4);
            if ((long)w > 0) {
                for (uint32_t y = 0; y < g_screen_h; y++)
                    zcopy32(&w[y * g_screen_w], &g_fb[y * g_fb_pitch_pixels], g_screen_w);
                g_wall = w;
            }
        }
    }

    /* ---- Wallpaper: /wall.png if present, else the bootloader gradient
       stays underneath. Decoded with the same self-contained inflate+defilter
       pipeline the terminal's image viewer uses (zerp_png.h). Nearest-
       neighbor stretch to the full screen -- no filtering, honest v1. ---- */
    {
        static uint8_t  wall_file[512 * 1024];
        static uint32_t wall_rgba[1920 * 1080];

        long wfd = zopen("/wall.png", O_RDONLY);
        if (wfd >= 0) {
            long total = 0;
            while (total < (long)sizeof(wall_file)) {
                long n = zread((int)wfd, wall_file + total, sizeof(wall_file) - total);
                if (n <= 0) break;
                total += n;
            }
            zclose(wfd);
            if (total > 8) {
                ZerpPngInfo inf = zerp_png_decode(wall_file, (uint32_t)total,
                                                  wall_rgba,
                                                  sizeof(wall_rgba) / 4);
                if (inf.ok && inf.width > 0 && inf.height > 0) {
                    for (uint32_t y2 = 0; y2 < g_screen_h; y2++) {
                        uint32_t sy = y2 * inf.height / g_screen_h;
                        const uint32_t *srow = &wall_rgba[sy * inf.width];
                        uint32_t *drow = g_wall ? &g_wall[y2 * g_screen_w]
                                                : &g_fb[y2 * g_fb_pitch_pixels];
                        for (uint32_t x2 = 0; x2 < g_screen_w; x2++) {
                            drow[x2] = srow[x2 * inf.width / g_screen_w] | 0xFF000000;
                        }
                    }
                    zwrite(1, "[zerp] wallpaper applied\n", 26);
                } else {
                    zwrite(1, "[zerp] wallpaper decode failed\n", 32);
                }
            }
        }
    }

    g_shm_bytes = g_screen_w * g_screen_h * 4;
    spawn_client("/zerp_files.elf", g_shm_bytes);
    spawn_client("/zerp_term.elf", g_shm_bytes);
    /* Qt Quick demo (Zerp 2.0 groundwork), when the image has Qt 6 */
    {
        long qfd = zopen("/usr/bin/qmldemo", 0);
        if (qfd >= 0) {
            zclose(qfd);
            spawn_client("/usr/bin/qmldemo", g_shm_bytes);
        }
    }

    retile();
    repaint_layout();
    screen_present();
    zwrite(1, "[zerp] clients spawned and tiled\n", 34);

    /* Pointer input comes from the kernel as absolute events (x, y,
       buttons after each PS/2 packet): the same IRQ drives the hardware
       cursor plane, so hit-testing always matches what's on screen. The
       first event is the pointer's current position. */
    uint8_t last_buttons = 0;
    int mouse_err_logged = 0;
    /* Which client got each key's make code (index = scancode & 0x7F,
       +128 for E0-prefixed keys), so its break code goes to the same
       client even after focus moved; -1 = none. */
    int8_t key_owner[256];
    for (int k = 0; k < 256; k++) key_owner[k] = -1;
    int kbd_e0 = 0;
    int focused = 0;
    uint64_t next_frame = 0; /* in 1/ZERP_HZ ms units */
#if ZERP_STATS
    uint64_t stat_t0 = zclock_ms();
    uint32_t stat_frames = 0, stat_damage = 0, stat_loops = 0;
#endif

    for (;;) {
        /* -- mouse: absolute events from the kernel. Pure motion is
           coalesced into ONE message per iteration; a button change is
           delivered immediately with the position it happened at, so no
           click is ever lost or misplaced. */
        {
            int moved = 0;
            int32_t mx = 0, my = 0;
            ZMouseEvent ev[64];
            long n;
            while ((n = zmouse_events(ev, 64)) > 0) {
                for (long k = 0; k < n; k++) {
                    uint8_t b = (uint8_t)(ev[k].buttons & 0x07);
                    mx = ev[k].x; my = ev[k].y;
                    if (b != last_buttons) {
                        last_buttons = b;
                        deliver_mouse(mx, my, b, 0, &focused);
                        moved = 0;
                    } else {
                        moved = 1;
                    }
                }
                if (n < 64) break;
            }
            if (n < 0 && !mouse_err_logged) {
                /* -EBUSY: another process owns the pointer stream */
                zwrite(1, "[zerp] no pointer input: SYS_mouse_events failed (pointer owned by another process?)\n", 85);
                mouse_err_logged = 1;
            }
            if (moved) deliver_mouse(mx, my, last_buttons, 1, &focused);
        }

        /* -- keyboard: raw set-1 scancodes. A make code goes to the
           focused client; its break code goes wherever the make went, so
           a key held while focus moves still gets released. */
        {
            uint8_t kb[64];
            long n;
            while ((n = zread((int)kbd_fd, kb, sizeof(kb))) > 0) {
                for (long k = 0; k < n; k++) {
                    uint8_t sc = kb[k];
                    if (sc == 0xE0) { kbd_e0 = 1; continue; }
                    int key = (sc & 0x7F) + (kbd_e0 ? 128 : 0);
                    int target = focused;
                    if (sc == 0xE1 || sc == 0xFA || sc == 0xFE) {
                        /* Pause prefix / controller replies: pass through */
                    } else if (sc & 0x80) {
                        if (key_owner[key] >= 0) target = key_owner[key];
                        key_owner[key] = -1;
                    } else if (key_owner[key] >= 0) {
                        /* typematic repeat of a held key: stays with the
                           client that got the first make */
                        target = key_owner[key];
                    } else {
                        key_owner[key] = (int8_t)focused;
                    }
                    if (target < g_client_count && g_clients[target].alive) {
                        client_send_key(&g_clients[target], kbd_e0, sc);
                    }
                    kbd_e0 = 0;
                }
                if (n < (long)sizeof(kb)) break;
            }
        }

        /* -- client control messages: DAMAGE/CLOSE/SPAWN. Every message
           starts with a 4-byte type field that fixes its size (ZerpMsg, or
           ZerpSpawnMsg for SPAWN). c2s is non-blocking, so bytes are
           accumulated in the client's rx buffer and only whole messages
           are parsed; a partial tail waits for the next iteration. DAMAGE
           is only RECORDED here; the blit happens once per frame below. */
        int need_retile = 0;
        for (int i = 0; i < g_client_count; i++) {
            ClientSlot *c = &g_clients[i];
            if (!c->alive) continue;
            client_flush_out(c);
            for (;;) {
                long n = zread(c->c2s_read_fd, c->rx + c->rx_len, sizeof(c->rx) - c->rx_len);
                if (n <= 0) break;
                c->rx_len += (uint32_t)n;

                uint32_t off = 0;
                while (c->alive && c->rx_len - off >= sizeof(uint32_t)) {
                    uint32_t type;
                    memcpy(&type, c->rx + off, sizeof(type));
                    uint32_t size = (type == ZERP_MSG_SPAWN) ? sizeof(ZerpSpawnMsg) : sizeof(ZerpMsg);
                    if (c->rx_len - off < size) break;
                    const uint8_t *body = c->rx + off + sizeof(uint32_t);
                    off += size;

                    if (type == ZERP_MSG_SPAWN) {
                        char path[252];
                        memcpy(path, body, sizeof(path));
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
                            focused = new_idx; g_focused_client = focused;
                        }
                    } else {
                        uint32_t rest[4];
                        memcpy(rest, body, sizeof(rest));
                        if (type == ZERP_MSG_DAMAGE) {
                            client_add_damage(c, rest[0], rest[1], rest[2], rest[3]);
#if ZERP_STATS
                            stat_damage++;
#endif
                        } else if (type == ZERP_MSG_CLOSE) {
                            c->alive = 0;
                            need_retile = 1;
                        }
                    }
                }
                /* keep the unparsed tail at the front */
                if (off > 0) {
                    uint32_t left = c->rx_len - off;
                    for (uint32_t k = 0; k < left; k++) c->rx[k] = c->rx[off + k];
                    c->rx_len = left;
                }
                if (!c->alive) break;
            }
        }

        if (need_retile) {
            compact_clients();
            /* client indices shift: held keys now release to the focus */
            for (int k = 0; k < 256; k++) key_owner[k] = -1;
            if (focused >= g_client_count) focused = g_client_count > 0 ? g_client_count - 1 : 0;
            g_focused_client = focused;
            retile();
            repaint_layout();
        }

        /* -- present: at most ZERP_HZ per second, carrying every
           client's merged damage and any chrome changes. */
        uint64_t now = zclock_ms();
        if (now * ZERP_HZ >= next_frame) {
            for (int i = 0; i < g_client_count; i++) {
                if (g_clients[i].alive) blit_client_damage(i);
            }
            if (g_scr_damaged) {
                screen_present();
                next_frame += 1000;             /* one frame = 1000/ZERP_HZ ms */
                if (next_frame < now * ZERP_HZ) /* idle gap: don't burst-catch-up */
                    next_frame = now * ZERP_HZ;
#if ZERP_STATS
                stat_frames++;
#endif
            }
        }

#if ZERP_STATS
        stat_loops++;
        if (now - stat_t0 >= 1000) {
            char nb[16];
            zwrite(1, "[zerp] fps=", 11);
            znum_to_str((long)(stat_frames * 1000ull / (now - stat_t0)), nb); zwrite(1, nb, zstrlen(nb));
            zwrite(1, " dmg/s=", 7);
            znum_to_str(stat_damage, nb); zwrite(1, nb, zstrlen(nb));
            zwrite(1, " loops/s=", 9);
            znum_to_str(stat_loops, nb); zwrite(1, nb, zstrlen(nb));
            zwrite(1, "\n", 1);
            stat_t0 = now; stat_frames = stat_damage = stat_loops = 0;
        }
#endif

        /* A real 1ms sleep (the kernel parks us; nothing spins). Input is
           still picked up within ~1ms, and the pointer itself never waits
           on this loop at all -- it's the GPU cursor plane. */
        zsleep_ms(1);
    }
}

#include "zerp_entry.h"
