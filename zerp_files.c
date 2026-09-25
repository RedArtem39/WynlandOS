/*
 * WynlandOS / Zerp - file manager.
 * Lists the current directory via the already-working SYS_getdents64
 * (kernel/syscall.c:1379+, wrapping real vfs_getdents()/drivers/fs/vfs.c
 * -- no new kernel work needed for this). Mouse click (Zerp's proven
 * ZERP_MSG_INPUT_MOUSE routing) on a row navigates into directories; the
 * first row is always ".." (except at "/"). Text rendered via
 * zerp_font.h's 8x16 bitmap font.
 *
 * Build (see zerp.c's header for the full non-PIE flag rationale):
 *   x86_64-linux-musl-gcc -nostdlib -static -no-pie -fno-pie \
 *     -mcmodel=large -O2 -Wl,-Ttext-segment=0x390000000000 \
 *     -o zerp_files.elf zerp_files.c
 */
#include "zerp_font.h"

#define MAX_ENTRIES 64
#define MAX_NAME 64
#define ROW_H (ZERP_FONT_HEIGHT + 2)
#define BG_COLOR   0xFF1A1A2A
#define DIR_COLOR  0xFF66CCFF
#define FILE_COLOR 0xFFDDDDDD
#define HDR_COLOR  0xFF888888

struct linux_dirent64 {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

typedef struct {
    char name[MAX_NAME];
    int is_dir;
} Entry;

static char g_path[256] = "/";
static Entry g_entries[MAX_ENTRIES];
static int g_entry_count = 0;

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Append " / name" (or just "name" if at root) onto g_path in place. */
static void path_push(const char *name) {
    int len = zstrlen(g_path);
    if (len > 1 || g_path[0] != '/') { g_path[len++] = '/'; }
    else if (g_path[0] == '/' && len == 1) { /* root: no extra slash needed */ }
    int i = 0;
    while (name[i] && len < (int)sizeof(g_path) - 1) { g_path[len++] = name[i++]; }
    g_path[len] = '\0';
}

/* Strip the last path component off g_path in place ("/a/b" -> "/a"). */
static void path_pop(void) {
    int len = zstrlen(g_path);
    if (len <= 1) return; /* already at root */
    len--;
    while (len > 0 && g_path[len] != '/') len--;
    if (len == 0) len = 1; /* keep the leading slash */
    g_path[len] = '\0';
}

static void list_directory(void) {
    g_entry_count = 0;
    long fd = zopen(g_path, O_RDONLY);
    if (fd < 0) return;

    static uint8_t buf[4096];
    long n = zgetdents64((int)fd, buf, sizeof(buf));
    zclose((int)fd);
    if (n <= 0) return;

    long off = 0;
    while (off < n && g_entry_count < MAX_ENTRIES) {
        struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
        if (d->d_reclen == 0) break;
        if (!(d->d_name[0] == '.' && (d->d_name[1] == '\0' ||
              (d->d_name[1] == '.' && d->d_name[2] == '\0')))) {
            str_copy(g_entries[g_entry_count].name, d->d_name, MAX_NAME);
            g_entries[g_entry_count].is_dir = (d->d_type == 4);
            g_entry_count++;
        }
        off += d->d_reclen;
    }
}

static void redraw(ZerpClient *zc) {
    zerp_fill(zc, BG_COLOR);
    zerp_draw_string_bg(zc, 4, 2, g_path, HDR_COLOR, BG_COLOR);

    int row = 1; /* row 0 reserved for the path header */
    int has_up = (zstrlen(g_path) > 1);
    if (has_up) {
        zerp_draw_string_bg(zc, 4, (uint32_t)(row * ROW_H), "[DIR] ..", DIR_COLOR, BG_COLOR);
        row++;
    }
    for (int i = 0; i < g_entry_count; i++) {
        char line[MAX_NAME + 8];
        const char *prefix = g_entries[i].is_dir ? "[DIR] " : "      ";
        int p = 0;
        while (prefix[p]) { line[p] = prefix[p]; p++; }
        str_copy(line + p, g_entries[i].name, MAX_NAME);
        zerp_draw_string_bg(zc, 4, (uint32_t)(row * ROW_H),
                             line, g_entries[i].is_dir ? DIR_COLOR : FILE_COLOR, BG_COLOR);
        row++;
    }
    zerp_send_damage(zc, 0, 0, zc->tile_w, zc->tile_h);
}

int zerp_main(int argc, char **argv) {
    ZerpClient zc;
    if (zerp_connect(argc, argv, &zc, 2048u * 2048u * 4u) != 0) return 1;
    if (zerp_wait_for_tile(&zc, 100000) != 0) return 1;

    list_directory();
    redraw(&zc);

    for (;;) {
        ZerpMsg msg;
        while (zerp_poll_server_msg(&zc, &msg)) {
            if (msg.type == ZERP_MSG_TILE_RECT) {
                redraw(&zc);
            } else if (msg.type == ZERP_MSG_INPUT_MOUSE) {
                if (msg.w & 0x01) { /* left click */
                    int clicked_row = (int)(msg.y / ROW_H);
                    if (clicked_row == 0) continue; /* clicked the path header */
                    int has_up = (zstrlen(g_path) > 1);
                    int idx = clicked_row - 1;
                    if (has_up) {
                        if (idx == 0) {
                            path_pop();
                            list_directory();
                            redraw(&zc);
                            continue;
                        }
                        idx--;
                    }
                    if (idx >= 0 && idx < g_entry_count && g_entries[idx].is_dir) {
                        path_push(g_entries[idx].name);
                        list_directory();
                        redraw(&zc);
                    }
                }
            }
        }
        /* Block until Zerp sends something: an idle window costs no CPU. */
        zpoll_in(zc.s2c_fd, -1);
    }
}

#include "zerp_entry.h"
