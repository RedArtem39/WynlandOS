/*
 * WynlandOS - Swap: private anonymous pages out to a disk partition
 * =================================================================
 * The swap partition (MBR type 0x82, at the end of the disk image) is cut
 * into 4 KB slots. When free RAM runs low, pages nobody touched lately --
 * private, not shared with anyone (frame refcount 1), not copy-on-write --
 * go out to a slot, and their page-table entry becomes a swap entry: not
 * present, PAGE_SWAPPED, slot + 1 in the address bits. Touching it faults
 * the page back into a fresh frame (vma_fault_in(), kernel/vma.c). fork()
 * gives the child the same slot (one more reference); unmapping or exiting
 * drops it. The access rights a page comes back with are its VMA's, not
 * the entry's (mprotect() needn't know about pages in swap).
 *
 * All of it in the big kernel lock (the page tables are its) and the
 * filesystem's mutex (the disk has one command slot).
 */
#pragma once

#include <wynland/types.h>
#include <wynland/vmm.h>

#define PAGE_SWAPPED (1ULL << 58)   /* software bit, only in a not-present leaf */

static inline bool pte_swapped(uint64_t e)
{
    return !(e & PAGE_PRESENT) && (e & PAGE_SWAPPED);
}

static inline uint32_t pte_swap_slot(uint64_t e)
{
    return (uint32_t)(((e & PAGE_ADDR_MASK) >> 12) - 1);
}

static inline uint64_t swap_pte(uint32_t slot)
{
    return PAGE_SWAPPED | ((uint64_t)(slot + 1) << 12);
}

struct Process;
struct VMA;

/* the partition (kernel/main.c, from drivers/fs/mbr.c); 0 sectors: none */
void swap_init(uint32_t lba, uint32_t sectors);
/* the reclaim thread (after the scheduler) */
void swap_start_daemon(void);
bool swap_enabled(void);

void swap_dup(uint32_t slot);
void swap_free(uint32_t slot);

/* p's entry for `page` is a swap entry: the page back in, mapped as v
   allows. False only when it cannot be (no memory, a disk error). */
bool swap_in(struct Process *p, const struct VMA *v, uint64_t page);

/* free about `want` pages by sending some out (the big lock held); how
   many went. For an allocation that found no memory at all. */
uint32_t swap_reclaim(uint32_t want);

/* bytes: the partition, and what is free in it (/proc/meminfo, sysinfo) */
void swap_stats(uint64_t *total, uint64_t *free);
