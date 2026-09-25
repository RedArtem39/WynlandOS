/*
 * WynlandOS / Zerp - client library.
 * Header-only (all `static`) so each demo app stays a single-file,
 * freestanding, no-libc translation unit -- matches this repo's existing
 * test-binary convention (spawner.c, test_raw_fb.c, shm_test_*.c).
 *
 * A client never opens /dev/fb0, /dev/input/mice, or /dev/input/kbd
 * directly -- Zerp owns those exclusively (documented convention, same
 * as Phase 1/2's "don't run gui and a Ring-3 fb0 writer at once"). A
 * client's only view of the display is the SHM buffer Zerp handed it at
 * spawn time (argv[1]=c2s_fd, argv[2]=s2c_fd, argv[3]=shm_fd -- all
 * inherited fds, positional per process_spawn()'s fd-inheritance), and
 * its only input is what Zerp forwards over the s2c pipe once it's the
 * focused window.
 */
#ifndef ZERP_CLIENT_H
#define ZERP_CLIENT_H

#include "zerp_syscalls.h"
#include "zerp_protocol.h"

typedef struct {
    int c2s_fd, s2c_fd, shm_fd;
    uint32_t *shm;
    uint32_t shm_capacity_pixels; /* upper bound the SHM segment can hold */
    uint32_t tile_x, tile_y, tile_w, tile_h; /* current assigned screen tile */
} ZerpClient;

/* argv must be {path, "<c2s_fd>", "<s2c_fd>", "<shm_fd>", NULL} -- exactly
   what Zerp's per-client spawn sequence hands over (see zerp.c). Returns
   0 on success, -1 on failure (missing argv, bad mmap). */
static int zerp_connect(int argc, char **argv, ZerpClient *zc, uint32_t shm_capacity_bytes) {
    if (argc < 4) return -1;

    zc->c2s_fd = (int)zstrtol(argv[1]);
    zc->s2c_fd = (int)zstrtol(argv[2]);
    zc->shm_fd = (int)zstrtol(argv[3]);
    /* zerp_poll_server_msg() is a non-blocking drain; clients that want
       to wait use zpoll_in() instead. */
    zfcntl(zc->s2c_fd, F_SETFL, O_NONBLOCK);
    zc->tile_x = zc->tile_y = 0;
    zc->tile_w = zc->tile_h = 0;

    zc->shm = (uint32_t *)zmmap_shm(zc->shm_fd, (long)shm_capacity_bytes);
    if ((long)zc->shm <= 0) return -1;

    zc->shm_capacity_pixels = shm_capacity_bytes / 4;
    return 0;
}

/* Blocks (via zyield()-spin) up to `max_tries` iterations for the first
   TILE_RECT from Zerp, so a client has a valid tile before it tries to
   draw. Zerp always sends one immediately after spawning a client. */
static int zerp_wait_for_tile(ZerpClient *zc, int max_tries) {
    for (int i = 0; i < max_tries; i++) {
        ZerpMsg msg;
        long n = zread(zc->s2c_fd, &msg, sizeof(msg));
        if (n == (long)sizeof(msg) && msg.type == ZERP_MSG_TILE_RECT) {
            zc->tile_x = msg.x; zc->tile_y = msg.y;
            zc->tile_w = msg.w; zc->tile_h = msg.h;
            return 0;
        }
        zyield();
    }
    return -1;
}

static void zerp_fill_rect(ZerpClient *zc, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (zc->tile_w == 0) return;
    if (x >= zc->tile_w || y >= zc->tile_h) return;
    if (x + w > zc->tile_w) w = zc->tile_w - x;
    if (y + h > zc->tile_h) h = zc->tile_h - y;
    for (uint32_t row = 0; row < h; row++) {
        uint32_t *dst = &zc->shm[(y + row) * zc->tile_w + x];
        for (uint32_t col = 0; col < w; col++) dst[col] = color;
    }
}

static void zerp_fill(ZerpClient *zc, uint32_t color) {
    zerp_fill_rect(zc, 0, 0, zc->tile_w, zc->tile_h, color);
}

static void zerp_send_damage(ZerpClient *zc, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    ZerpMsg msg = { ZERP_MSG_DAMAGE, x, y, w, h };
    zwrite(zc->c2s_fd, &msg, sizeof(msg));
}

static void zerp_send_close(ZerpClient *zc) {
    ZerpMsg msg = { ZERP_MSG_CLOSE, 0, 0, 0, 0 };
    zwrite(zc->c2s_fd, &msg, sizeof(msg));
}

/* Asks Zerp to launch a new client at `path` (e.g. a launcher's Enter
   key). See zerp_protocol.h's ZerpSpawnMsg comment for why this is a
   differently-sized message than the others sent over c2s. */
static void zerp_send_spawn(ZerpClient *zc, const char *path) {
    ZerpSpawnMsg msg;
    msg.type = ZERP_MSG_SPAWN;
    int i = 0;
    while (path[i] != '\0' && i < (int)sizeof(msg.path) - 1) { msg.path[i] = path[i]; i++; }
    msg.path[i] = '\0';
    zwrite(zc->c2s_fd, &msg, sizeof(msg));
}

/* Non-blocking. Returns 1 and fills *out if a message was waiting,
   0 otherwise. Auto-applies TILE_RECT updates to zc's own state before
   returning it, since almost every caller needs that regardless. */
static int zerp_poll_server_msg(ZerpClient *zc, ZerpMsg *out) {
    long n = zread(zc->s2c_fd, out, sizeof(ZerpMsg));
    if (n != (long)sizeof(ZerpMsg)) return 0;
    if (out->type == ZERP_MSG_TILE_RECT) {
        zc->tile_x = out->x; zc->tile_y = out->y;
        zc->tile_w = out->w; zc->tile_h = out->h;
    }
    return 1;
}

#endif /* ZERP_CLIENT_H */
