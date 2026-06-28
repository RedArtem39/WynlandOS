/*
 * WynlandOS - Virtual Memory Manager (VMM) Header
 */

#pragma once

#include <wynland/types.h>
#include <wynland/boot_info.h>

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITE    (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_WRITE_THROUGH (1ULL << 3)  /* PWT bit */
#define PAGE_CACHE_DISABLE (1ULL << 4)  /* PCD bit */
#define PAGE_PAT      (1ULL << 7)  /* PAT bit */
#define PAGE_NX       0ULL         /* Disabled to prevent hypervisor crashes when EFER.NXE is not enabled */
#define PAGE_ADDR_MASK 0x000FFFFFFFFFF000ULL


#define PML4_INDEX(addr) (((addr) >> 39) & 0x1FF)
#define PDPT_INDEX(addr) (((addr) >> 30) & 0x1FF)
#define PD_INDEX(addr)   (((addr) >> 21) & 0x1FF)
#define PT_INDEX(addr)   (((addr) >> 12) & 0x1FF)

typedef uint64_t PageTableEntry;

typedef struct {
    PageTableEntry entries[512];
} PageTable;

/* Initialize the VMM: sets up a new PML4 page table structure and loads it into CR3 */
void vmm_init(BootInfo *boot_info);

/* Map a virtual page to a physical page with specific flags */
void vmm_map_page(PageTable *pml4, uint64_t virt, uint64_t phys, uint64_t flags);

/* Unmap a virtual page */
void vmm_unmap_page(PageTable *pml4, uint64_t virt);

/* Get the current PML4 page table root */
PageTable *vmm_get_current_pml4(void);

/* Get physical address of a virtual address by traversing page tables */
uint64_t vmm_get_phys(PageTable *pml4, uint64_t virt);

/* Map a memory-mapped I/O (MMIO) region */
void vmm_map_mmio(uint64_t phys_addr, uint64_t size);
