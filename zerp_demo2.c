/*
 * WynlandOS / Zerp - demo client #2: mouse-reactive.
 * Fills its assigned tile with a solid color; while its tile has the
 * cursor and the left button is held, draws a small white square at the
 * cursor position. Proves mouse input routing (position translated into
 * tile-local coordinates by Zerp) + damage-driven redraw.
 * Build (see zerp.c's header for the full non-PIE flag rationale):
 *   x86_64-linux-musl-gcc -nostdlib -static -no-pie -fno-pie \
 *     -mcmodel=large -O2 -Wl,-Ttext-segment=0x360000000000 \
 *     -o zerp_demo2.elf zerp_demo2.c
 */
#include "zerp_client.h"

#define BG_COLOR  0xFF7733CC
#define DOT_COLOR 0xFFFFFFFF
#define DOT_SIZE  12

int zerp_main(int argc, char **argv) {
    ZerpClient zc;
    if (zerp_connect(argc, argv, &zc, 2048u * 2048u * 4u) != 0) return 1;
    if (zerp_wait_for_tile(&zc, 100000) != 0) return 1;

    zerp_fill(&zc, BG_COLOR);
    zerp_send_damage(&zc, 0, 0, zc.tile_w, zc.tile_h);

    for (;;) {
        ZerpMsg msg;
        while (zerp_poll_server_msg(&zc, &msg)) {
            if (msg.type == ZERP_MSG_TILE_RECT) {
                zerp_fill(&zc, BG_COLOR);
                zerp_send_damage(&zc, 0, 0, zc.tile_w, zc.tile_h);
            } else if (msg.type == ZERP_MSG_INPUT_MOUSE) {
                if (msg.w & 0x01) { /* left button held */
                    uint32_t dx = (msg.x > DOT_SIZE / 2) ? msg.x - DOT_SIZE / 2 : 0;
                    uint32_t dy = (msg.y > DOT_SIZE / 2) ? msg.y - DOT_SIZE / 2 : 0;
                    zerp_fill_rect(&zc, dx, dy, DOT_SIZE, DOT_SIZE, DOT_COLOR);
                    zerp_send_damage(&zc, dx, dy, DOT_SIZE, DOT_SIZE);
                }
            }
        }
        zyield();
    }
}

#include "zerp_entry.h"
