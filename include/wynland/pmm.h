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

/* Free a previously allocated physical page */
void pmm_free_page(void *addr);

/* Utility functions to get memory stats */
uint64_t pmm_get_total_memory(void);
uint64_t pmm_get_free_memory(void);
uint64_t pmm_get_used_memory(void);
