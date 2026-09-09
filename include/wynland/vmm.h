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
#define PAGE_PS       (1ULL << 7)  /* Huge Page size bit on PD/PDPT */
#define PAGE_NX       0ULL         /* Disabled to prevent hypervisor crashes when EFER.NXE is not enabled */
#define PAGE_COW      (1ULL << 9)  /* Software-defined bit (ignored by the CPU on x86_64 -- bits 9-11 of a
                                       present leaf entry are architecturally free for OS use). Marks a leaf
                                       that fork()'s COW sharing (vmm_cow_clone_user_pages()) mapped read-only
                                       into two-or-more processes; the page fault handler (kernel/idt.c) checks
                                       this bit before falling back to its fatal path on a write fault. */
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

/* Get the kernel's boot-time PML4 (kernel code + identity-mapped RAM + framebuffer) */
PageTable *vmm_get_kernel_pml4(void);

/* Allocate a fresh PML4 for a new process, pre-populated with the kernel's
   mappings (aliased by pointer, not deep-copied) so kernel code/interrupts/
   syscalls keep working once this table is loaded into CR3. */
PageTable *vmm_new_process_pml4(void);

/* Get physical address of a virtual address by traversing page tables */
uint64_t vmm_get_phys(PageTable *pml4, uint64_t virt);

/* True if the mapping at virt has PAGE_USER set (i.e. genuinely owned by
   this process, not an aliased kernel-identity-map page it happens to
   overlap -- see vmm_new_process_pml4()). Used by the ELF loader to refuse
   writing user segment content over shared kernel memory it doesn't own. */
bool vmm_is_user_page(PageTable *pml4, uint64_t virt);

/* Map a memory-mapped I/O (MMIO) region */
void vmm_map_mmio(uint64_t phys_addr, uint64_t size);

/* Deep-copy every PAGE_USER leaf mapping from parent_pml4 into child_pml4,
   each with its own fresh physical page (eager copy, not COW). Superseded
   by vmm_cow_clone_user_pages() for real fork() as of Phase 22c; kept for
   reference/fallback use. Returns false on OOM. */
bool vmm_clone_user_pages(PageTable *parent_pml4, PageTable *child_pml4);

/* Phase 22c: real fork() support. Shares every PAGE_USER leaf mapping from
   parent_pml4 into child_pml4 BY PHYSICAL PAGE (no copy) -- both the parent's
   and the child's PTE for that page get PAGE_WRITE cleared and PAGE_COW set,
   and the underlying frame's refcount (see pmm_page_incref()) is bumped so
   neither side frees it out from under the other. A later write fault from
   either process resolves through the page fault handler's COW path
   (kernel/idt.c), which either reclaims the frame outright (refcount drops
   to 1, i.e. this faulter is the last owner) or duplicates it. Returns false
   on OOM (table allocation failure only -- sharing itself can't fail). */
bool vmm_cow_clone_user_pages(PageTable *parent_pml4, PageTable *child_pml4);

/* Change the permission flags of an already-present leaf mapping in place,
   preserving its physical address. `flags` is the complete replacement set
   (PAGE_USER, PAGE_WRITE, PAGE_COW as desired -- PAGE_PRESENT is forced on).
   Returns false if `virt` has no present leaf mapping (nothing to protect --
   callers with an eager-allocation model shouldn't hit this in practice).
   Flushes the TLB for this address on success. */
bool vmm_protect_page(PageTable *pml4, uint64_t virt, uint64_t flags);

/* Raw flags (PAGE_PRESENT|PAGE_WRITE|PAGE_USER|PAGE_COW, etc.) of the leaf
   mapping at virt, or 0 if unmapped. Used by the page fault handler to tell
   a real permission violation apart from a COW-pending write. */
uint64_t vmm_get_page_flags(PageTable *pml4, uint64_t virt);
