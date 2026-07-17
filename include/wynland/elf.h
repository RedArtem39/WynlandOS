/*
 * WynlandOS - ELF64 Loader Header
 */
#pragma once
#include <wynland/types.h>
#include <wynland/vmm.h>

#define MAX_LOADED_PAGES 1024

typedef struct {
    void *phys_pages[MAX_LOADED_PAGES];
    uint64_t virt_addrs[MAX_LOADED_PAGES];
    int count;
} LoadedPages;

/*
 * Load a statically or dynamically linked ELF64 binary.
 * Maps code, data, and BSS segments into pml4, allocates a stack,
 * and returns the entry point and stack top.
 * Tracks all allocated pages in out_pages for subsequent reclamation.
 */
bool elf_load(const char *path, uint64_t *out_entry, uint64_t *out_stack_top, PageTable *pml4, LoadedPages *out_pages);
