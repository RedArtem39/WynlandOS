/*
 * WynlandOS - Physical Memory Manager (PMM) Header
 */

#pragma once

#include <wynland/types.h>
#include <wynland/boot_info.h>

#define PAGE_SIZE 4096

/* Initialize the PMM using the memory map from BootInfo */
void pmm_init(BootInfo *boot_info);

/* Allocate a single physical page (4KB). Returns physical address or NULL */
void *pmm_alloc_page(void);
void *pmm_alloc_contiguous(uint32_t count);

/* Free a previously allocated physical page. If the frame is still shared
   (refcount > 1, see pmm_page_incref()) this only drops one reference and
   keeps the frame allocated for the remaining owner(s); the frame is only
   actually returned to the free bitmap once the last reference drops. */
void pmm_free_page(void *addr);

/* Add one reference to an already-allocated frame -- used by fork()'s COW
   sharing (vmm_cow_clone_user_pages()) when a physical page gets aliased
   into a second process's address space instead of copied. */
void pmm_page_incref(void *addr);

/* Current reference count of an allocated frame (0 if not allocated). A
   freshly allocated, unshared page has refcount 1. */
uint32_t pmm_page_refcount(void *addr);

/* Utility functions to get memory stats */
uint64_t pmm_get_total_memory(void);
uint64_t pmm_get_free_memory(void);
uint64_t pmm_get_used_memory(void);
