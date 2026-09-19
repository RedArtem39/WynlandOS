/*
 * WynlandOS - ELF64 Loader Header
 */
#pragma once
#include <wynland/types.h>
#include <wynland/vmm.h>

struct Process; /* include/wynland/process.h -- forward-declared; elf_load()
                    only needs a pointer to register real VMAs (Phase 22c) */

/*
 * Load a statically or dynamically linked ELF64 binary.
 * Maps code, data, and BSS segments into pml4, allocates a stack,
 * and returns the entry point and stack top.
 *
 * On failure, everything this call mapped into `pml4` is unwound via
 * vmm_free_user_mappings() (kernel/vmm.c) before returning false -- a walk
 * of the actual page tables, not a fixed-size tracking array (the old
 * LoadedPages/MAX_LOADED_PAGES=1024 cap here meant a binary needing more
 * pages than that -- the 8MB stack alone is 2048 -- silently stopped being
 * tracked partway through, so a later failure couldn't find or free
 * everything it had mapped). No size limit on what can be rolled back.
 */
/* envp: NULL preserves the historical hardcoded 5-var default
   (XDG_RUNTIME_DIR/LD_LIBRARY_PATH/WLR_*), same NULL-means-default
   convention as argv. A real NULL-terminated array fully replaces it --
   needed by Phase 18's execve() so a spawned child can receive a real
   TERM=vt100 (required for ncurses' setupterm() to pick the right
   escape-sequence set). */
bool elf_load(const char *path, uint64_t *out_entry, uint64_t *out_stack_top, PageTable *pml4, const char **argv, const char **envp, struct Process *proc);
