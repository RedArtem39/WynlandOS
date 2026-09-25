/*
 * WynlandOS / Zerp - demo client #1: keyboard-reactive.
 * Fills its assigned tile with a solid color; each keypress routed to it
 * (i.e. while its tile has the cursor) cycles to the next color. Proves
 * keyboard input routing + damage-driven redraw through the real
 * multi-process Zerp pipeline (no Qt6, no kernel GUI code involved).
 * Build (see zerp.c's header for the full non-PIE flag rationale):
 *   x86_64-linux-musl-gcc -nostdlib -static -no-pie -fno-pie \
 *     -mcmodel=large -O2 -Wl,-Ttext-segment=0x350000000000 \
 *     -o zerp_demo1.elf zerp_demo1.c
 */
#include "zerp_client.h"

static uint32_t g_colors[4] = { 0xFFFF3333, 0xFF33FF33, 0xFF3333FF, 0xFFFFFF33 };

static void redraw(ZerpClient *zc, int color_idx) {
    zerp_fill(zc, g_colors[color_idx]);
    zerp_send_damage(zc, 0, 0, zc->tile_w, zc->tile_h);
}

int zerp_main(int argc, char **argv) {
    ZerpClient zc;
    if (zerp_connect(argc, argv, &zc, 2048u * 2048u * 4u) != 0) return 1;
    if (zerp_wait_for_tile(&zc, 100000) != 0) return 1;

    int color_idx = 0;
    redraw(&zc, color_idx);

    for (;;) {
        ZerpMsg msg;
        while (zerp_poll_server_msg(&zc, &msg)) {
            if (msg.type == ZERP_MSG_INPUT_KEY) {
                color_idx = (color_idx + 1) % 4;
                redraw(&zc, color_idx);
            } else if (msg.type == ZERP_MSG_TILE_RECT) {
                redraw(&zc, color_idx);
            }
        }
        zpoll_in(zc.s2c_fd, -1); /* block until the compositor has news */
    }
}

#include "zerp_entry.h"
