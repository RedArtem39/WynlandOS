/*
 * WynlandOS / Zerp - client<->compositor control protocol.
 * One fixed-size 20-byte struct shape for both directions, comfortably
 * under the kernel's 4KB pipe buffer in a single read()/write() call --
 * no chunking needed (see the plan's "Pixel transport" note: pipes are
 * fine for small control messages, just not for bulk pixel data, which
 * is why that goes over shared memory instead, see zerp_client.h).
 */
#ifndef ZERP_PROTOCOL_H
#define ZERP_PROTOCOL_H

#include <stdint.h> /* compiler-provided freestanding header, safe with -nostdlib */

/* client -> compositor (over the client's c2s pipe) */
#define ZERP_MSG_DAMAGE 1   /* x,y,w,h = dirty rect in the client's own SHM buffer */
#define ZERP_MSG_CLOSE  2   /* client is exiting; fields unused */
#define ZERP_MSG_SPAWN  6   /* path = binary to launch as a new Zerp client (ZerpSpawnMsg shape, not ZerpMsg) */

/* compositor -> client (over the client's s2c pipe) */
#define ZERP_MSG_TILE_RECT   3   /* x,y,w,h = this client's newly (re)assigned screen tile */
#define ZERP_MSG_INPUT_KEY   4   /* x = scancode, rest unused */
#define ZERP_MSG_INPUT_MOUSE 5   /* x,y = position within the client's tile, w = buttons mask */

typedef struct {
    uint32_t type;
    uint32_t x, y, w, h;
} ZerpMsg;

/* ZERP_MSG_SPAWN's wire shape -- deliberately NOT the same size as ZerpMsg
   (a path string doesn't fit in 4 uint32s). The compositor's c2s read loop
   reads the leading `type` field first (same 4 bytes either struct starts
   with), then reads the REST of the message at a size depending on what
   that type turned out to be -- see zerp.c's client-message loop. Both
   structs are written by the sender in one zwrite() call each, so the
   split read on the compositor side never straddles a partial write. */
typedef struct {
    uint32_t type;
    char path[252];
} ZerpSpawnMsg;

#endif /* ZERP_PROTOCOL_H */
