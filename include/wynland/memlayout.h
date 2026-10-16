/*
 * WynlandOS - where the kernel lives in the address space
 *
 * The lower half (0 .. 0x00007FFFFFFFFFFF) belongs to the running process
 * alone; nothing of the kernel is mapped there (except the SMP trampoline
 * page, in the kernel's own PML4 only). The upper half is the kernel's and
 * is the same in every process (vmm_new_process_pml4() copies entries
 * 256..511, all created by vmm_init() so they never change later):
 *
 *   0xFFFF800000000000  HHDM_BASE: all physical memory, VA = HHDM_BASE +
 *                       physical address (RAM by vmm_init(), device memory
 *                       by vmm_map_mmio()); 64 TB, PML4 entries 256..383
 *   0xFFFFC00000000000  KHEAP_BASE: the kernel heap (kernel/heap.c)
 *   0xFFFFFFFF80000000  KERNEL_VMA: the kernel image, VA = KERNEL_VMA +
 *                       physical address (loaded at 1 MB, kernel/linker.ld)
 *
 * Page-frame numbers (pmm_alloc_page(), page-table entries, device DMA)
 * are physical; the kernel reaches a frame through P2V().
 */

#pragma once

#include <wynland/types.h>

#define HHDM_BASE   0xFFFF800000000000ULL
#define HHDM_SIZE   (1ULL << 46)
#define KHEAP_BASE  0xFFFFC00000000000ULL
#define KERNEL_VMA  0xFFFFFFFF80000000ULL
#define KERNEL_PHYS 0x100000ULL

/* the kernel's view of a physical address */
static inline void *P2V(uint64_t phys)
{
    return (void *)(uintptr_t)(phys + HHDM_BASE);
}

/* a kernel heap address: by its page tables (kernel/vmm.c) */
uint64_t vmm_kernel_heap_phys(uint64_t virt);

/* the physical address behind a kernel pointer (direct map, kernel image
   or kernel heap) -- what a device's DMA or a page-table entry needs */
static inline uint64_t V2P(const void *ptr)
{
    uint64_t v = (uint64_t)(uintptr_t)ptr;
    if (v >= KERNEL_VMA) return v - KERNEL_VMA;
    if (v >= HHDM_BASE && v < HHDM_BASE + HHDM_SIZE) return v - HHDM_BASE;
    return vmm_kernel_heap_phys(v);
}
