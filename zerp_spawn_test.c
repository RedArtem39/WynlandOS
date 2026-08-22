/*
 * WynlandOS / Zerp - Phase 8 verification client.
 * Fills its tile cyan immediately, waits a short spin-delay (so an early
 * screendump can catch the "before" state), then sends a runtime
 * ZERP_MSG_SPAWN request for /zerp_demo2.elf -- proving Zerp can grow its
 * client list live, no reboot, via spawn_client() called from the main
 * loop's message-handling code (not just zerp_main()'s own boot-time
 * calls). Keeps redrawing on TILE_RECT so it stays visible through the
 * retile() that follows the new client joining.
 * Build (see zerp.c's header for the full non-PIE flag rationale):
 *   x86_64-linux-musl-gcc -nostdlib -static -no-pie -fno-pie \
 *     -mcmodel=large -O2 -Wl,-Ttext-segment=0x360000000000 \
 *     -o zerp_spawn_test.elf zerp_spawn_test.c
 */
#include "zerp_client.h"

static void redraw(ZerpClient *zc) {
    zerp_fill(zc, 0xFF00FFFF); /* cyan */
    zerp_send_damage(zc, 0, 0, zc->tile_w, zc->tile_h);
}

int zerp_main(int argc, char **argv) {
    ZerpClient zc;
    if (zerp_connect(argc, argv, &zc, 2048u * 2048u * 4u) != 0) return 1;
    if (zerp_wait_for_tile(&zc, 100000) != 0) return 1;

    redraw(&zc);
    zerp_send_spawn(&zc, "/zerp_demo2.elf");

    /* Close self the moment a TILE_RECT arrives in THIS loop (the very
       first TILE_RECT -- the initial connect tile -- was already
       consumed by zerp_wait_for_tile() above, before this loop starts;
       so the first one seen here is the retile() triggered by demo2
       joining). Event-driven, not a spin-count timing guess (which
       proved too slow/unreliable under this kernel's per-syscall
       overhead in earlier attempts at this same test). */
    for (;;) {
        ZerpMsg msg;
        while (zerp_poll_server_msg(&zc, &msg)) {
            if (msg.type == ZERP_MSG_TILE_RECT) {
                redraw(&zc);
                zerp_send_close(&zc);
            }
        }
        zyield();
    }
}

#include "zerp_entry.h"
