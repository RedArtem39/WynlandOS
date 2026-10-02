#ifndef WYNLAND_RANDOM_H
#define WYNLAND_RANDOM_H

#include <wynland/types.h>

/* Kernel CSPRNG (kernel/random.c): getrandom(), /dev/urandom, TCP
   sequence numbers and ports, DNS ids, password salts. */
void     random_bytes(void *buf, uint64_t len);
uint32_t random_u32(void);

#endif
