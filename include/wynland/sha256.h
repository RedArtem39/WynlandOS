#ifndef WYNLAND_SHA256_H
#define WYNLAND_SHA256_H

#include <wynland/types.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    uint32_t fill;
} Sha256;

void sha256_init(Sha256 *c);
void sha256_update(Sha256 *c, const void *data, uint64_t n);
void sha256_final(Sha256 *c, uint8_t out[32]);

#endif
