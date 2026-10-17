/*
 * WynlandOS - Virtual Memory Manager (VMM) Implementation
 */

#include <wynland/vmm.h>
#include <wynland/swap.h>
#include <wynland/pmm.h>
#include <wynland/boot_info.h>
#include <wynland/smp.h>
#include <wynland/memlayout.h>

extern uint8_t __kernel_end[];
extern void *memset(void *s, int c, size_t n);
extern void *memcpy(void *dest, const void *src, size_t n);
extern void serial_write_string(const char *str);

/* The kernel's PML4: its upper half (entries 256..511: the kernel image,
   the direct map of physical memory, the heap, device memory; include/
   wynland/memlayout.h) is built by vmm_init() and copied into every new
   process's PML4, so the kernel stays reachable after a CR3 switch into a
   process. Its lower half holds only the SMP start-up page (kernel/smp.c). */
static PageTable *g_kernel_pml4 = NULL;

/* Helper to allocate and zero a new page table */
static PageTable *vmm_alloc_table(void)
{
    void *phys = pmm_alloc_page();
    if (!phys) {
        serial_write_string("VMM Error: Out of physical memory for page table!\r\n");
        return NULL;
    }
    PageTable *t = (PageTable *)P2V((uint64_t)(uintptr_t)phys);
    memset(t, 0, PAGE_SIZE);
    return t;
}

PageTable *vmm_get_kernel_pml4(void)
{
    return g_kernel_pml4;
}

void vmm_load_cr3(PageTable *pml4)
{
    uint64_t phys = V2P(pml4);
    this_cpu()->cr3 = phys;
    __asm__ volatile("mov %0, %%cr3" :: "r"(phys) : "memory");
}

/* ---- the other cores' TLBs (include/wynland/smp.h) ----------------------
   A page-table entry that was present changed: the local invlpg is not
   enough when other cores run the same address space (threads of one
   process) or the change is in the kernel part every process shares --
   they may still hold the old translation and write through it, into a
   frame already freed. tlb_shootdown() makes them flush first.

   Batches (munmap, mprotect, fork's COW marking): one shootdown at the
   end instead of an IPI round trip per page. The frames such a batch
   frees wait for it too (vmm_free_page_after_flush()): until the other
   cores have flushed, a stale translation could still write into them. */
static uint32_t g_tlb_batch;
static bool g_tlb_dirty, g_tlb_all;
static PageTable *g_tlb_target;
#define TLB_DEFER_MAX 256
static void *g_tlb_defer[TLB_DEFER_MAX];
static uint32_t g_tlb_defer_n;

static void tlb_batch_flush(void)
{
    if (g_tlb_dirty) tlb_shootdown(g_tlb_all ? NULL : g_tlb_target);
    g_tlb_dirty = g_tlb_all = false;
    g_tlb_target = NULL;
    for (uint32_t i = 0; i < g_tlb_defer_n; i++) pmm_free_page(g_tlb_defer[i]);
    g_tlb_defer_n = 0;
}

static void tlb_remote(PageTable *pml4, uint64_t virt)
{
    if (g_cpu_count <= 1) return;
    uint64_t i4 = PML4_INDEX(virt);
    bool shared = pml4 == g_kernel_pml4 ||
                  (g_kernel_pml4 && pml4->entries[i4] == g_kernel_pml4->entries[i4]);
    if (!g_tlb_batch) {
        tlb_shootdown(shared ? NULL : pml4);
        return;
    }
    if (shared || (g_tlb_dirty && g_tlb_target != pml4)) g_tlb_all = true;
    g_tlb_target = pml4;
    g_tlb_dirty = true;
}

void vmm_tlb_batch_begin(void)
{
    g_tlb_batch++;
}

void vmm_tlb_batch_end(void)
{
    if (g_tlb_batch && --g_tlb_batch == 0) tlb_batch_flush();
}

void vmm_free_page_after_flush(void *frame)
{
    if (!g_tlb_batch || g_cpu_count <= 1) { pmm_free_page(frame); return; }
    if (g_tlb_defer_n == TLB_DEFER_MAX) tlb_batch_flush();
    g_tlb_defer[g_tlb_defer_n++] = frame;
}

PageTable *vmm_new_process_pml4(void)
{
    PageTable *pml4 = vmm_alloc_table();
    if (!pml4) return NULL;

    /* The kernel half: the kernel PML4's entries 256..511 by value -- the
       same PDPTs (made once in vmm_init(), never replaced), so what is
       mapped there later is seen here too. They carry no PAGE_USER: ring 3
       reaches nothing below them. The lower half starts empty: the
       process's alone. */
    for (int i = 256; i < 512; i++) {
        if (g_kernel_pml4->entries[i] & PAGE_PRESENT) {
            pml4->entries[i] = g_kernel_pml4->entries[i];
        }
    }
    return pml4;
}

bool vmm_clone_user_pages(PageTable *parent_pml4, PageTable *child_pml4)
{
    /* Real fork() support: deep-copy every PAGE_USER leaf mapping from the
       parent's address space into the child's, with its own fresh physical
       page (eager copy, not copy-on-write -- simpler and self-contained,
       no page-fault-handler changes needed; this OS doesn't have fork-heavy
       workloads yet, so the extra memcpy cost isn't worth the COW
       complexity/risk right now).

       Only walks PRESENT entries at each level (cheap in practice -- bounded
       by how much is actually mapped, not the full 512^4 address space), and
       only acts on leaves with PAGE_USER set, which automatically skips the
       kernel half every process's PML4 shares (vmm_new_process_pml4(); never
       PAGE_USER) -- no special-casing needed to avoid re-copying it. */
    for (uint64_t i4 = 0; i4 < 512; i4++) {
        if (!(parent_pml4->entries[i4] & PAGE_PRESENT)) continue;
        PageTable *pdpt = (PageTable *)P2V(parent_pml4->entries[i4] & PAGE_ADDR_MASK);

        for (uint64_t i3 = 0; i3 < 512; i3++) {
            if (!(pdpt->entries[i3] & PAGE_PRESENT)) continue;
            if (pdpt->entries[i3] & PAGE_PS) continue; /* 1GB huge page -- never produced for user mappings by vmm_map_page, skip defensively */
            PageTable *pd = (PageTable *)P2V(pdpt->entries[i3] & PAGE_ADDR_MASK);

            for (uint64_t i2 = 0; i2 < 512; i2++) {
                if (!(pd->entries[i2] & PAGE_PRESENT)) continue;
                if (pd->entries[i2] & PAGE_PS) continue; /* 2MB huge page -- same as above */
                PageTable *pt = (PageTable *)P2V(pd->entries[i2] & PAGE_ADDR_MASK);

                for (uint64_t i1 = 0; i1 < 512; i1++) {
                    uint64_t pte = pt->entries[i1];
                    if (pte_swapped(pte)) {
                        /* in swap (kernel/swap.c): the child gets the same
                           slot, one more reference -- whichever side
                           touches it first reads it into a page of its own */
                        uint64_t sv = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);
                        swap_dup(pte_swap_slot(pte));
                        vmm_set_pte_raw(child_pml4, sv, pte);
                        continue;
                    }
                    if (!(pte & PAGE_PRESENT)) continue;
                    if (!(pte & PAGE_USER)) continue;

                    uint64_t phys  = pte & PAGE_ADDR_MASK;
                    uint64_t flags = (pte & 0xFFFULL) & ~PAGE_PRESENT; /* vmm_map_page ORs PRESENT in itself */

                    uint64_t virt = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);
                    if (i4 & 0x100) virt |= 0xFFFF000000000000ULL; /* canonical sign-extend, defensive -- this OS's own user addresses never actually reach the upper half today */

                    if (pte & PAGE_SHARED_MAP) {
                        /* MAP_SHARED / device memory stays shared, exactly
                           as in vmm_cow_clone_user_pages(): same frame,
                           same flags, one more reference if the mapping
                           holds its own (memfd). */
                        if (pte & PAGE_SHARED_REF) pmm_page_incref((void *)(uintptr_t)phys);
                        vmm_map_page(child_pml4, virt, phys, (pte & ~PAGE_ADDR_MASK) & ~PAGE_PRESENT);
                        continue;
                    }

                    /* Keep NX (bit 63; `flags` holds only the low 12 bits),
                       and a private copy has no reason to stay COW. */
                    flags |= pte & PAGE_NX;
                    if (flags & PAGE_COW) flags = (flags & ~PAGE_COW) | PAGE_WRITE;

                    void *new_phys = pmm_alloc_page();
                    if (!new_phys) {
                        serial_write_string("vmm_clone_user_pages: out of physical memory\r\n");
                        return false;
                    }
                    memcpy(P2V((uint64_t)(uintptr_t)new_phys), P2V(phys), PAGE_SIZE);
                    vmm_map_page(child_pml4, virt, (uint64_t)(uintptr_t)new_phys, flags);
                }
            }
        }
    }
    return true;
}

bool vmm_cow_clone_user_pages(PageTable *parent_pml4, PageTable *child_pml4)
{
    /* Same PML4/PDPT/PD/PT walk as vmm_clone_user_pages() above, but instead
       of allocating a fresh frame + memcpy per leaf, both parent and child
       end up pointing at the SAME frame, read-only, with PAGE_COW set. The
       actual duplication (or outright reclaim, if this faulter turns out to
       be the last owner) happens lazily in the page fault handler. */
    vmm_tlb_batch_begin();   /* the parent's other threads: one flush at the end */
    for (uint64_t i4 = 0; i4 < 512; i4++) {
        if (!(parent_pml4->entries[i4] & PAGE_PRESENT)) continue;
        PageTable *pdpt = (PageTable *)P2V(parent_pml4->entries[i4] & PAGE_ADDR_MASK);

        for (uint64_t i3 = 0; i3 < 512; i3++) {
            if (!(pdpt->entries[i3] & PAGE_PRESENT)) continue;
            if (pdpt->entries[i3] & PAGE_PS) continue;
            PageTable *pd = (PageTable *)P2V(pdpt->entries[i3] & PAGE_ADDR_MASK);

            for (uint64_t i2 = 0; i2 < 512; i2++) {
                if (!(pd->entries[i2] & PAGE_PRESENT)) continue;
                if (pd->entries[i2] & PAGE_PS) continue;
                PageTable *pt = (PageTable *)P2V(pd->entries[i2] & PAGE_ADDR_MASK);

                for (uint64_t i1 = 0; i1 < 512; i1++) {
                    uint64_t pte = pt->entries[i1];
                    if (pte_swapped(pte)) {
                        /* in swap (kernel/swap.c): the child gets the same
                           slot, one more reference -- whichever side
                           touches it first reads it into a page of its own */
                        uint64_t sv = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);
                        swap_dup(pte_swap_slot(pte));
                        vmm_set_pte_raw(child_pml4, sv, pte);
                        continue;
                    }
                    if (!(pte & PAGE_PRESENT)) continue;
                    if (!(pte & PAGE_USER)) continue;

                    uint64_t phys  = pte & PAGE_ADDR_MASK;
                    uint64_t flags = (pte & 0xFFFULL) & ~PAGE_PRESENT;

                    uint64_t virt = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);
                    if (i4 & 0x100) virt |= 0xFFFF000000000000ULL;

                    if (pte & PAGE_SHARED_MAP) {
                        /* MAP_SHARED / device memory: the child sees the
                           same frame, writable as before, never COW. Its
                           owner object (SHM segment, DRM BO, memfd)
                           decides its lifetime; a mapping that holds its
                           own pmm reference (PAGE_SHARED_REF) takes one
                           more for the child's copy. */
                        if (pte & PAGE_SHARED_REF) pmm_page_incref((void *)(uintptr_t)phys);
                        vmm_map_page(child_pml4, virt, phys,
                                     (pte & ~PAGE_ADDR_MASK) & ~PAGE_PRESENT);
                        continue;
                    }

                    /* If this leaf was writable, it's a real COW candidate:
                       both sides lose PAGE_WRITE and gain PAGE_COW, and a
                       write fault from either resolves through the page
                       fault handler. If it was ALREADY read-only (e.g. an
                       mprotect(PROT_READ) region), just share the frame
                       as-is with no COW tag -- a write fault there is a
                       genuine permission violation on both sides, not
                       something to resolve by duplicating. Either way this
                       is exactly one new owner of the frame (the child), so
                       exactly one incref. */
                    uint64_t new_flags = (flags & PAGE_WRITE) ? ((flags & ~PAGE_WRITE) | PAGE_COW) : flags;

                    pmm_page_incref((void *)(uintptr_t)phys);

                    pt->entries[i1] = phys | new_flags | PAGE_PRESENT;
                    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
                    tlb_remote(parent_pml4, virt);

                    vmm_map_page(child_pml4, virt, phys, new_flags);
                }
            }
        }
    }
    vmm_tlb_batch_end();
    return true;
}

void vmm_free_user_mappings(PageTable *pml4)
{
    if (!pml4) return;
    PageTable *kernel_pml4 = vmm_get_kernel_pml4();

    /* Same PML4/PDPT/PD/PT walk as vmm_clone_user_pages()/
       vmm_cow_clone_user_pages() above, but freeing instead of copying or
       sharing. Only walks PRESENT entries (cheap -- bounded by how much is
       actually mapped) and only frees a top-level branch that ISN'T shared
       with the kernel snapshot -- see this function's own header comment
       in vmm.h for exactly why that comparison is safe. */
    for (uint64_t i4 = 0; i4 < 256; i4++) {     /* the user half */
        uint64_t pml4_entry = pml4->entries[i4];
        if (!(pml4_entry & PAGE_PRESENT)) continue;
        if (kernel_pml4 && pml4_entry == kernel_pml4->entries[i4]) continue; /* shared -- never touch */

        PageTable *pdpt = (PageTable *)P2V(pml4_entry & PAGE_ADDR_MASK);

        for (uint64_t i3 = 0; i3 < 512; i3++) {
            if (!(pdpt->entries[i3] & PAGE_PRESENT)) continue;
            if (pdpt->entries[i3] & PAGE_PS) continue; /* 1GB huge page -- never produced for a private branch, skip defensively */
            PageTable *pd = (PageTable *)P2V(pdpt->entries[i3] & PAGE_ADDR_MASK);

            for (uint64_t i2 = 0; i2 < 512; i2++) {
                if (!(pd->entries[i2] & PAGE_PRESENT)) continue;
                if (pd->entries[i2] & PAGE_PS) continue; /* 2MB huge page -- same as above */
                PageTable *pt = (PageTable *)P2V(pd->entries[i2] & PAGE_ADDR_MASK);

                for (uint64_t i1 = 0; i1 < 512; i1++) {
                    uint64_t pte = pt->entries[i1];
                    if (pte_swapped(pte)) { swap_free(pte_swap_slot(pte)); continue; }
                    if (!(pte & PAGE_PRESENT)) continue;
                    /* owned by its SHM/BO/fb object -- unless this mapping
                       holds its own reference (memfd), which it drops */
                    if ((pte & PAGE_SHARED_MAP) && !(pte & PAGE_SHARED_REF)) continue;
                    /* Refcount-aware: a leaf still COW-shared with a sibling
                       process (fork()'d, never written since) just drops
                       this owner's share here instead of freeing outright. */
                    pmm_free_page((void *)(uintptr_t)(pte & PAGE_ADDR_MASK));
                }
                pmm_free_page((void *)V2P(pt)); /* privately owned -- this whole branch isn't shared */
            }
            pmm_free_page((void *)V2P(pd));
        }
        pmm_free_page((void *)V2P(pdpt));
        pml4->entries[i4] = 0;
    }
    /* Another core still running these tables (a thread of the process
       an execve() replaces) must not keep using them. Nothing reuses the
       freed frames before this returns. */
    if (g_cpu_count > 1) tlb_shootdown(pml4);
}

void vmm_destroy_process_pml4(PageTable *pml4)
{
    if (!pml4) return;
    vmm_free_user_mappings(pml4);
    pmm_free_page((void *)V2P(pml4));
}

void vmm_map_page(PageTable *pml4, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

    /* Round addresses to page boundaries */
    virt &= ~(PAGE_SIZE - 1);
    phys &= ~(PAGE_SIZE - 1);

    uint64_t pml4_idx = PML4_INDEX(virt);
    uint64_t pdpt_idx = PDPT_INDEX(virt);
    uint64_t pd_idx   = PD_INDEX(virt);
    uint64_t pt_idx   = PT_INDEX(virt);

    /* 1. Traverse PML4 -> PDPT */
    PageTableEntry *pml4_entry = &pml4->entries[pml4_idx];
    PageTable *pdpt = NULL;
    if (!(*pml4_entry & PAGE_PRESENT)) {
        pdpt = vmm_alloc_table();
        *pml4_entry = V2P(pdpt) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    } else {
        pdpt = (PageTable *)P2V(*pml4_entry & PAGE_ADDR_MASK);
    }

    /* 2. Traverse PDPT -> PD */
    PageTableEntry *pdpt_entry = &pdpt->entries[pdpt_idx];
    PageTable *pd = NULL;
    if (!(*pdpt_entry & PAGE_PRESENT)) {
        pd = vmm_alloc_table();
        *pdpt_entry = V2P(pd) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    } else {
        pd = (PageTable *)P2V(*pdpt_entry & PAGE_ADDR_MASK);
    }

    /* 3. Traverse PD -> PT */
    PageTableEntry *pd_entry = &pd->entries[pd_idx];
    PageTable *pt = NULL;
    if (!(*pd_entry & PAGE_PRESENT)) {
        pt = vmm_alloc_table();
        *pd_entry = V2P(pt) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    } else {
        pt = (PageTable *)P2V(*pd_entry & PAGE_ADDR_MASK);
    }

    /* 4. Map the physical address in the Page Table */
    bool was_present = (pt->entries[pt_idx] & PAGE_PRESENT) != 0;
    pt->entries[pt_idx] = phys | flags | PAGE_PRESENT;

    // if (virt >= 0x700000000000ULL) {
    //     serial_write_string("VMM Map debug: ");
    //     char buf[32];
    //     extern void uint_to_hex(uint64_t val, char *buf);
    //     extern void uint_to_str(uint64_t val, char *buf);
    //     uint_to_hex(virt, buf); serial_write_string(buf); serial_write_string(" -> ");
    //     uint_to_hex(phys, buf); serial_write_string(buf); serial_write_string(" (pt=");
    //     uint_to_hex((uint64_t)(uintptr_t)pt, buf); serial_write_string(buf); serial_write_string(" idx=");
    //     uint_to_str(pt_idx, buf); serial_write_string(buf); serial_write_string(")\r\n");
    // }

    /* 5. Invalidate TLB for this virtual address (other cores too, when
       it replaced a mapping: nothing caches a not-present entry) */
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    if (was_present) tlb_remote(pml4, virt);

    if (rflags & 0x200) {
        __asm__ volatile("sti\n\tnop" ::: "memory");
    }
}

void vmm_unmap_page(PageTable *pml4, uint64_t virt)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

    virt &= ~(PAGE_SIZE - 1);
    uint64_t pml4_idx = PML4_INDEX(virt);
    uint64_t pdpt_idx = PDPT_INDEX(virt);
    uint64_t pd_idx   = PD_INDEX(virt);
    uint64_t pt_idx   = PT_INDEX(virt);

    PageTableEntry *pml4_entry = &pml4->entries[pml4_idx];
    if (!(*pml4_entry & PAGE_PRESENT)) goto cleanup;
    
    PageTable *pdpt = (PageTable *)P2V(*pml4_entry & PAGE_ADDR_MASK);
    PageTableEntry *pdpt_entry = &pdpt->entries[pdpt_idx];
    if (!(*pdpt_entry & PAGE_PRESENT)) goto cleanup;

    PageTable *pd = (PageTable *)P2V(*pdpt_entry & PAGE_ADDR_MASK);
    PageTableEntry *pd_entry = &pd->entries[pd_idx];
    if (!(*pd_entry & PAGE_PRESENT)) goto cleanup;

    PageTable *pt = (PageTable *)P2V(*pd_entry & PAGE_ADDR_MASK);
    
    /* Clear entry (a page in swap: its slot goes) */
    bool was_present = (pt->entries[pt_idx] & PAGE_PRESENT) != 0;
    if (pte_swapped(pt->entries[pt_idx])) swap_free(pte_swap_slot(pt->entries[pt_idx]));
    pt->entries[pt_idx] = 0;

    /* Invalidate TLB (here and on the other cores) */
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    if (was_present) tlb_remote(pml4, virt);

cleanup:
    if (rflags & 0x200) {
        __asm__ volatile("sti\n\tnop" ::: "memory");
    }
}

bool vmm_protect_page(PageTable *pml4, uint64_t virt, uint64_t flags)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

    bool ok = false;
    virt &= ~(PAGE_SIZE - 1);
    uint64_t pml4_idx = PML4_INDEX(virt);
    uint64_t pdpt_idx = PDPT_INDEX(virt);
    uint64_t pd_idx   = PD_INDEX(virt);
    uint64_t pt_idx   = PT_INDEX(virt);

    PageTableEntry *pml4_entry = &pml4->entries[pml4_idx];
    if (!(*pml4_entry & PAGE_PRESENT)) goto cleanup;

    PageTable *pdpt = (PageTable *)P2V(*pml4_entry & PAGE_ADDR_MASK);
    PageTableEntry *pdpt_entry = &pdpt->entries[pdpt_idx];
    if (!(*pdpt_entry & PAGE_PRESENT)) goto cleanup;

    PageTable *pd = (PageTable *)P2V(*pdpt_entry & PAGE_ADDR_MASK);
    PageTableEntry *pd_entry = &pd->entries[pd_idx];
    if (!(*pd_entry & PAGE_PRESENT)) goto cleanup;

    PageTable *pt = (PageTable *)P2V(*pd_entry & PAGE_ADDR_MASK);
    PageTableEntry *pt_entry = &pt->entries[pt_idx];
    if (!(*pt_entry & PAGE_PRESENT)) goto cleanup;

    uint64_t phys = *pt_entry & PAGE_ADDR_MASK;
    *pt_entry = phys | flags | PAGE_PRESENT;
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    tlb_remote(pml4, virt);
    ok = true;

cleanup:
    if (rflags & 0x200) {
        __asm__ volatile("sti\n\tnop" ::: "memory");
    }
    return ok;
}

uint64_t vmm_get_page_flags(PageTable *pml4, uint64_t virt)
{
    uint64_t pml4_idx = PML4_INDEX(virt);
    uint64_t pdpt_idx = PDPT_INDEX(virt);
    uint64_t pd_idx   = PD_INDEX(virt);
    uint64_t pt_idx   = PT_INDEX(virt);

    PageTableEntry *pml4_entry = &pml4->entries[pml4_idx];
    if (!(*pml4_entry & PAGE_PRESENT)) return 0;

    PageTable *pdpt = (PageTable *)P2V(*pml4_entry & PAGE_ADDR_MASK);
    PageTableEntry *pdpt_entry = &pdpt->entries[pdpt_idx];
    if (!(*pdpt_entry & PAGE_PRESENT)) return 0;

    PageTable *pd = (PageTable *)P2V(*pdpt_entry & PAGE_ADDR_MASK);
    PageTableEntry *pd_entry = &pd->entries[pd_idx];
    if (!(*pd_entry & PAGE_PRESENT)) return 0;

    PageTable *pt = (PageTable *)P2V(*pd_entry & PAGE_ADDR_MASK);
    PageTableEntry *pt_entry = &pt->entries[pt_idx];
    return *pt_entry & 0xFFFULL;
}

bool vmm_resolve_cow_page(PageTable *pml4, uint64_t page_addr)
{
    page_addr &= ~(PAGE_SIZE - 1);
    uint64_t flags = vmm_get_page_flags(pml4, page_addr);
    if (!(flags & PAGE_COW)) return true; /* not COW -- nothing to resolve */

    uint64_t phys = vmm_get_phys(pml4, page_addr);
    uint32_t refs = pmm_page_refcount((void *)(uintptr_t)phys);

    if (refs <= 1) {
        /* Last (or only) owner -- no one else can be looking at this
           frame, so just reclaim write access in place, no copy needed. */
        vmm_protect_page(pml4, page_addr, (flags & ~PAGE_COW) | PAGE_WRITE);
        return true;
    }

    void *new_phys = pmm_alloc_page();
    if (!new_phys) return false; /* OOM duplicating a still-shared page */

    memcpy(P2V((uint64_t)(uintptr_t)new_phys), P2V(phys), PAGE_SIZE);
    vmm_map_page(pml4, page_addr, (uint64_t)(uintptr_t)new_phys, (flags & ~PAGE_COW) | PAGE_WRITE);
    pmm_free_page((void *)(uintptr_t)phys); /* drop this owner's share of the old, still-shared frame */
    return true;
}

uint64_t vmm_get_pte(PageTable *pml4, uint64_t virt)
{
    PageTableEntry e = pml4->entries[PML4_INDEX(virt)];
    if (!(e & PAGE_PRESENT)) return 0;
    PageTable *pdpt = (PageTable *)P2V(e & PAGE_ADDR_MASK);
    e = pdpt->entries[PDPT_INDEX(virt)];
    if (!(e & PAGE_PRESENT) || (e & PAGE_PS)) return 0;
    PageTable *pd = (PageTable *)P2V(e & PAGE_ADDR_MASK);
    e = pd->entries[PD_INDEX(virt)];
    if (!(e & PAGE_PRESENT) || (e & PAGE_PS)) return 0;
    PageTable *pt = (PageTable *)P2V(e & PAGE_ADDR_MASK);
    e = pt->entries[PT_INDEX(virt)];
    return (e & PAGE_PRESENT) ? e : 0;
}

uint64_t vmm_get_pte_raw(PageTable *pml4, uint64_t virt)
{
    PageTableEntry e = pml4->entries[PML4_INDEX(virt)];
    if (!(e & PAGE_PRESENT)) return 0;
    PageTable *pdpt = (PageTable *)P2V(e & PAGE_ADDR_MASK);
    e = pdpt->entries[PDPT_INDEX(virt)];
    if (!(e & PAGE_PRESENT) || (e & PAGE_PS)) return 0;
    PageTable *pd = (PageTable *)P2V(e & PAGE_ADDR_MASK);
    e = pd->entries[PD_INDEX(virt)];
    if (!(e & PAGE_PRESENT) || (e & PAGE_PS)) return 0;
    PageTable *pt = (PageTable *)P2V(e & PAGE_ADDR_MASK);
    return pt->entries[PT_INDEX(virt)];
}

PageTableEntry *vmm_pte_ptr(PageTable *pml4, uint64_t virt)
{
    PageTableEntry e = pml4->entries[PML4_INDEX(virt)];
    if (!(e & PAGE_PRESENT)) return NULL;
    PageTable *pdpt = (PageTable *)P2V(e & PAGE_ADDR_MASK);
    e = pdpt->entries[PDPT_INDEX(virt)];
    if (!(e & PAGE_PRESENT) || (e & PAGE_PS)) return NULL;
    PageTable *pd = (PageTable *)P2V(e & PAGE_ADDR_MASK);
    e = pd->entries[PD_INDEX(virt)];
    if (!(e & PAGE_PRESENT) || (e & PAGE_PS)) return NULL;
    PageTable *pt = (PageTable *)P2V(e & PAGE_ADDR_MASK);
    return &pt->entries[PT_INDEX(virt)];
}

void vmm_set_pte_raw(PageTable *pml4, uint64_t virt, uint64_t entry)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));
    virt &= ~(PAGE_SIZE - 1);
    PageTableEntry *e = &pml4->entries[PML4_INDEX(virt)];
    for (int level = 0; level < 3; level++) {
        if (!(*e & PAGE_PRESENT)) {
            PageTable *t = vmm_alloc_table();
            *e = V2P(t) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
        }
        PageTable *t = (PageTable *)P2V(*e & PAGE_ADDR_MASK);
        uint64_t idx = level == 0 ? PDPT_INDEX(virt) : level == 1 ? PD_INDEX(virt) : PT_INDEX(virt);
        e = &t->entries[idx];
    }
    bool was_present = (*e & PAGE_PRESENT) != 0;
    *e = entry;
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    if (was_present) tlb_remote(pml4, virt);
    if (rflags & 0x200) __asm__ volatile("sti\n\tnop" ::: "memory");
}

/* First address in [virt, end) that has a 4 KB leaf mapping, or end.
   Whole absent page-table levels are skipped in one step (512 GB, 1 GB,
   2 MB), so walking a huge, mostly empty range costs next to nothing --
   munmap(addr, 1 TB) used to loop page by page with interrupts off. */
uint64_t vmm_next_mapped(PageTable *pml4, uint64_t virt, uint64_t end)
{
    virt &= ~(uint64_t)(PAGE_SIZE - 1);
    while (virt < end) {
        uint64_t step;
        PageTableEntry e = pml4->entries[PML4_INDEX(virt)];
        if (!(e & PAGE_PRESENT)) { step = 1ULL << 39; goto skip; }
        PageTable *pdpt = (PageTable *)P2V(e & PAGE_ADDR_MASK);
        e = pdpt->entries[PDPT_INDEX(virt)];
        if (!(e & PAGE_PRESENT) || (e & PAGE_PS)) { step = 1ULL << 30; goto skip; }
        PageTable *pd = (PageTable *)P2V(e & PAGE_ADDR_MASK);
        e = pd->entries[PD_INDEX(virt)];
        if (!(e & PAGE_PRESENT) || (e & PAGE_PS)) { step = 1ULL << 21; goto skip; }
        PageTable *pt = (PageTable *)P2V(e & PAGE_ADDR_MASK);
        if (pt->entries[PT_INDEX(virt)] & (PAGE_PRESENT | PAGE_SWAPPED)) return virt;
        step = PAGE_SIZE;
    skip:
        {
            uint64_t next = (virt & ~(step - 1)) + step;
            if (next <= virt) return end;   /* wrapped */
            virt = next;
        }
    }
    return end;
}

uint64_t vmm_get_phys(PageTable *pml4, uint64_t virt)
{
    uint64_t pml4_idx = PML4_INDEX(virt);
    uint64_t pdpt_idx = PDPT_INDEX(virt);
    uint64_t pd_idx   = PD_INDEX(virt);
    uint64_t pt_idx   = PT_INDEX(virt);

    PageTableEntry *pml4_entry = &pml4->entries[pml4_idx];
    if (!(*pml4_entry & PAGE_PRESENT)) return 0;
    
    PageTable *pdpt = (PageTable *)P2V(*pml4_entry & PAGE_ADDR_MASK);
    PageTableEntry *pdpt_entry = &pdpt->entries[pdpt_idx];
    if (!(*pdpt_entry & PAGE_PRESENT)) return 0;
    if (*pdpt_entry & PAGE_PS) {
        /* 1GB huge page */
        return (*pdpt_entry & PAGE_ADDR_MASK) | (virt & 0x3FFFFFFFULL);
    }

    PageTable *pd = (PageTable *)P2V(*pdpt_entry & PAGE_ADDR_MASK);
    PageTableEntry *pd_entry = &pd->entries[pd_idx];
    if (!(*pd_entry & PAGE_PRESENT)) return 0;
    if (*pd_entry & PAGE_PS) {
        /* 2MB huge page */
        return (*pd_entry & PAGE_ADDR_MASK) | (virt & 0x1FFFFFULL);
    }

    PageTable *pt = (PageTable *)P2V(*pd_entry & PAGE_ADDR_MASK);
    PageTableEntry *pt_entry = &pt->entries[pt_idx];
    if (!(*pt_entry & PAGE_PRESENT)) return 0;

    return (*pt_entry & PAGE_ADDR_MASK) | (virt & (PAGE_SIZE - 1));
}

bool vmm_is_user_page(PageTable *pml4, uint64_t virt)
{
    uint64_t pml4_idx = PML4_INDEX(virt);
    uint64_t pdpt_idx = PDPT_INDEX(virt);
    uint64_t pd_idx   = PD_INDEX(virt);
    uint64_t pt_idx   = PT_INDEX(virt);

    PageTableEntry *pml4_entry = &pml4->entries[pml4_idx];
    if (!(*pml4_entry & PAGE_PRESENT)) return false;

    PageTable *pdpt = (PageTable *)P2V(*pml4_entry & PAGE_ADDR_MASK);
    PageTableEntry *pdpt_entry = &pdpt->entries[pdpt_idx];
    if (!(*pdpt_entry & PAGE_PRESENT)) return false;
    if (*pdpt_entry & PAGE_PS) return (*pdpt_entry & PAGE_USER) != 0;

    PageTable *pd = (PageTable *)P2V(*pdpt_entry & PAGE_ADDR_MASK);
    PageTableEntry *pd_entry = &pd->entries[pd_idx];
    if (!(*pd_entry & PAGE_PRESENT)) return false;
    if (*pd_entry & PAGE_PS) return (*pd_entry & PAGE_USER) != 0;

    PageTable *pt = (PageTable *)P2V(*pd_entry & PAGE_ADDR_MASK);
    PageTableEntry *pt_entry = &pt->entries[pt_idx];
    if (!(*pt_entry & PAGE_PRESENT)) return false;

    return (*pt_entry & PAGE_USER) != 0;
}

static inline uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void write_msr(uint32_t msr, uint64_t val) {
    uint32_t low = (uint32_t)val;
    uint32_t high = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" :: "a"(low), "d"(high), "c"(msr));
}

/* See the extern declaration/comment in include/wynland/vmm.h. 0 until
   vmm_nx_init() (called from vmm_init(), below, before any page is ever
   mapped) confirms the CPU supports NX and enables EFER.NXE. */
uint64_t g_page_nx_bit = 0;

#define MSR_EFER_LOCAL 0xC0000080ULL
#define EFER_NXE       (1ULL << 11)

static void vmm_nx_init(void)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001), "c"(0));

    if (!(edx & (1u << 20))) { /* CPUID.80000001H:EDX.NX */
        serial_write_string("VMM: CPU does not report NX support -- PAGE_NX stays a no-op.\r\n");
        return;
    }

    uint64_t efer = read_msr(MSR_EFER_LOCAL);
    efer |= EFER_NXE;
    write_msr(MSR_EFER_LOCAL, efer);

    g_page_nx_bit = (1ULL << 63);
    serial_write_string("VMM: NX supported -- EFER.NXE enabled.\r\n");
}

void vmm_init(BootInfo *boot_info)
{
    serial_write_string("VMM: Initializing paging...\r\n");

    /* Must run before the very first vmm_map_page() call below: this
       function's own non-usable-memory/framebuffer/MMIO mappings further
       down already OR in PAGE_NX (previously a permanent no-op) -- setting
       that bit in a live PTE before EFER.NXE is enabled would be an
       instant reserved-bit #PF on the next translation through it. */
    vmm_nx_init();

    /* Allocate and zero the root PML4 table */
    PageTable *pml4 = vmm_alloc_table();
    if (!pml4) {
        serial_write_string("VMM Error: Failed to allocate PML4 table!\r\n");
        return;
    }

    /* 1. The kernel half: one PDPT for each of PML4 entries 256..511, made
       now and never replaced -- every process's PML4 copies these entries
       (vmm_new_process_pml4()), so whatever is mapped in the upper half
       later (heap, device memory) is seen by all of them. No PAGE_USER at
       this level: user code can reach nothing below it. */
    for (int i = 256; i < 512; i++) {
        PageTable *pdpt = vmm_alloc_table();
        if (!pdpt) return;
        pml4->entries[i] = V2P(pdpt) | PAGE_PRESENT | PAGE_WRITE;
    }

    /* 2. The kernel image at KERNEL_VMA + its physical address */
    uint64_t kernel_start = KERNEL_PHYS;
    uint64_t kernel_end = ((uint64_t)__kernel_end - KERNEL_VMA + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t addr = kernel_start; addr < kernel_end; addr += PAGE_SIZE) {
        vmm_map_page(pml4, KERNEL_VMA + addr, addr, PAGE_WRITE);
    }

    /* 3. All memory from the UEFI memory map at HHDM_BASE + its address */
    MemoryRegion *regions = (MemoryRegion *)(uintptr_t)boot_info->mmap_addr;
    uint32_t region_count = boot_info->mmap_entries;

    for (uint32_t i = 0; i < region_count; i++) {
        /* Skip reserved and bad memory to avoid mapping unbacked/MMIO holes */
        if (regions[i].type == MEMORY_RESERVED || regions[i].type == MEMORY_BAD) {
            continue;
        }

        uint64_t base = regions[i].base;
        uint64_t size = regions[i].size;
        if (base >= HHDM_SIZE) continue;

        uint64_t flags = PAGE_WRITE | PAGE_NX;   /* data: the kernel runs from its image */
        for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE) {
            vmm_map_page(pml4, HHDM_BASE + base + offset, base + offset, flags);
        }
    }

    /* Configure PAT4 as Write-Combining (01h) in PAT MSR 0x277 */
    uint64_t pat = read_msr(0x277);
    pat &= ~(0xFFULL << 32); /* Clear PAT4 (bits 32..39) */
    pat |= (0x01ULL << 32);  /* Set PAT4 to WC (01h) */
    write_msr(0x277, pat);

    /* 4. The Graphics Output Protocol (GOP) framebuffer, write-combining */
    uint64_t fb_start = boot_info->fb_addr;
    uint64_t fb_size = boot_info->fb_pitch * boot_info->fb_height;
    fb_size = (fb_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t offset = 0; offset < fb_size; offset += PAGE_SIZE) {
        vmm_map_page(pml4, HHDM_BASE + fb_start + offset, fb_start + offset, PAGE_WRITE | PAGE_NX | PAGE_PAT);
    }

    /* Snapshot this as the kernel PML4 -- at this exact point it's
       guaranteed to contain only the kernel half, since no user process has
       run yet. New processes (see vmm_new_process_pml4()) copy its entries
       256..511. */
    g_kernel_pml4 = pml4;

    /* 5. Switch to our new PML4 page table by loading it into CR3 */
    vmm_load_cr3(pml4);

    serial_write_string("VMM: Switched to new page tables successfully!\r\n");
}

PageTable *vmm_get_current_pml4(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (PageTable *)P2V(cr3 & PAGE_ADDR_MASK);
}

void *vmm_map_mmio(uint64_t phys_addr, uint64_t size)
{
    /* in the kernel half, which every process shares: at P2V(phys_addr) */
    uint64_t start = phys_addr & ~(PAGE_SIZE - 1);
    uint64_t end = (phys_addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
        vmm_map_page(g_kernel_pml4, HHDM_BASE + addr, addr, PAGE_WRITE | PAGE_NX | PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH);
    }
    return P2V(phys_addr);
}

uint64_t vmm_kernel_heap_phys(uint64_t virt)
{
    return vmm_get_phys(g_kernel_pml4, virt);
}
