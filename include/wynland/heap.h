/*
 * WynlandOS - Kernel Heap Allocator Header
 */
#pragma once

#include <wynland/types.h>

#define HEAP_START 0x10000000ULL
#define HEAP_INITIAL_PAGES 512   /* 2 MB initial heap */
#define HEAP_MAX_PAGES 245760    /* 960 MB max heap */

#ifdef __cplusplus
extern "C" {
#endif

void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);

size_t heap_get_used_memory(void);
size_t heap_get_free_memory(void);
size_t heap_get_block_size(void *ptr);

#ifdef __cplusplus
}
#endif
