/*
 * WynlandOS - Virtual Memory Area (VMA) list
 * ============================================================
 * Phase 22c: per-process bookkeeping of which address ranges are
 * mapped, with what permissions, and why -- replaces the old
 * "map now, forget the range existed" model that made mprotect()/
 * munmap() no-ops. One list per Process (Process.vma_list).
 */
#pragma once

#include <wynland/types.h>

#define VMA_PROT_READ  0x1
#define VMA_PROT_WRITE 0x2
#define VMA_PROT_EXEC  0x4

#define VMA_ANON  0x1  /* anonymous (no backing file) */
#define VMA_GUARD 0x2  /* deliberately unmapped hole (e.g. below a stack) --
                           touching it is always fatal, never lazily faulted in */

struct Process; /* include/wynland/process.h -- forward-declared to avoid a
                    header cycle (Process embeds a VMA* list head) */

typedef struct VMA {
    uint64_t start;  /* page-aligned, inclusive */
    uint64_t end;    /* page-aligned, exclusive */
    uint32_t prot;   /* VMA_PROT_* */
    uint32_t flags;  /* VMA_ANON / VMA_GUARD */
    struct VMA *next;
} VMA;

/* Register a new region [start,end). Caller is responsible for the actual
   page mapping (or deliberately leaving it unmapped, for VMA_GUARD) --
   this only tracks the range for future mprotect/munmap/fault lookups.
   Does not check for overlap with existing VMAs; callers that need
   MAP_FIXED-replace semantics should vma_unmap_range() first. */
VMA *vma_insert(struct Process *proc, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags);

/* The VMA covering address `addr`, or NULL if none does. */
VMA *vma_find(struct Process *proc, uint64_t addr);

/* Update the prot field of every VMA overlapping [start,end), splitting
   VMAs at the boundary where the requested range only partially covers
   one. Returns false if any part of [start,end) isn't covered by an
   existing VMA (real mprotect() semantics: that's ENOMEM), but still
   applies the change to whatever portions ARE covered. Does not touch
   page tables -- caller applies the actual PTE flag change. */
bool vma_protect_range(struct Process *proc, uint64_t start, uint64_t end, uint32_t new_prot);

/* Remove VMA coverage of [start,end), splitting/trimming/deleting nodes as
   needed. Used by real munmap() and by mmap(MAP_FIXED)'s "replace whatever
   was there" semantics. Does not touch page tables or physical memory --
   caller unmaps/frees the actual pages. */
void vma_unmap_range(struct Process *proc, uint64_t start, uint64_t end);
