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
/* Real NX (No-Execute, PTE bit 63). Setting bit 63 in a page table entry
   while EFER.NXE is clear is a RESERVED-BIT violation -- the CPU page-faults
   immediately on the very next translation through that entry, regardless
   of whether the access was actually an instruction fetch. So this can't be
   a compile-time constant: vmm_init() checks CPUID.80000001H:EDX.NX (bit 20)
   and sets EFER.NXE before anything ever maps a page, storing the real bit
   in g_page_nx_bit only if the CPU actually supports it. Every existing
   `PAGE_NX`-using call site in the tree (vmm.c's own non-usable-memory/
   framebuffer/MMIO mappings, kernel/heap.c, gui/compositor.c, kernel/elf.c)
   keeps working unchanged either way: it degrades to a harmless 0 (today's
   permanent behavior) on the vanishingly unlikely chance the CPU doesn't
   report NX support, instead of guaranteeing an instant boot-time #PF. */
extern uint64_t g_page_nx_bit;
#define PAGE_NX g_page_nx_bit
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

/* Resolve a PAGE_COW leaf at `page_addr` (must already be page-aligned) into
   a real writable mapping: reclaims the frame in place if this is the last
   owner (refcount <= 1), otherwise duplicates it into a fresh frame and
   drops this owner's share of the old one. No-op (returns true) if the page
   isn't actually PAGE_COW. Returns false only on OOM duplicating a
   still-shared page. Shared by the page-fault handler's user-mode COW path
   (kernel/idt.c) and copy_to_user() (kernel/usercopy.c), which must resolve
   COW itself before writing since a CPL0 write fault on a COW page taken
   mid-syscall (CS still ring 0) does NOT hit that fault handler's ring-3-only
   fast path. */
bool vmm_resolve_cow_page(PageTable *pml4, uint64_t page_addr);

/* Frees every mapping this process PRIVATELY owns in `pml4`: every present
   leaf (via the refcount-aware pmm_free_page(), so a COW-shared leaf just
   drops this owner's share) and every PT/PD/PDPT page-table page under a
   top-level PML4 entry that ISN'T aliased from the shared kernel/RAM/
   framebuffer identity map (vmm_new_process_pml4() copies those entries by
   raw value from vmm_get_kernel_pml4()'s snapshot -- an entry that still
   matches that snapshot exactly is shared and is skipped entirely; anything
   else present was mapped later, specifically for this process, by
   vmm_map_page(), and is safe to free). Walks the actual page tables rather
   than any side list, so there is no size limit on how much can be torn
   down. Does NOT free the `pml4` root itself -- see
   vmm_destroy_process_pml4() for that, and elf_load()'s failure path
   (kernel/elf.c) for the other caller, which doesn't own the root. */
void vmm_free_user_mappings(PageTable *pml4);

/* vmm_free_user_mappings() plus the PML4 root page itself. Full address-
   space teardown for a process that's actually going away. */
void vmm_destroy_process_pml4(PageTable *pml4);
