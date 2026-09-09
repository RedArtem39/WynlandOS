/*
 * WynlandOS - ELF64 Loader Header
 */
#pragma once
#include <wynland/types.h>
#include <wynland/vmm.h>

struct Process; /* include/wynland/process.h -- forward-declared; elf_load()
                    only needs a pointer to register real VMAs (Phase 22c) */

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
/* envp: NULL preserves the historical hardcoded 5-var default
   (XDG_RUNTIME_DIR/LD_LIBRARY_PATH/WLR_*), same NULL-means-default
   convention as argv. A real NULL-terminated array fully replaces it --
   needed by Phase 18's execve() so a spawned child can receive a real
   TERM=vt100 (required for ncurses' setupterm() to pick the right
   escape-sequence set). */
bool elf_load(const char *path, uint64_t *out_entry, uint64_t *out_stack_top, PageTable *pml4, LoadedPages *out_pages, const char **argv, const char **envp, struct Process *proc);
