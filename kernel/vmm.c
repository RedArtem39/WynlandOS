/*
 * WynlandOS - Virtual Memory Manager (VMM) Implementation
 */

#include <wynland/vmm.h>
#include <wynland/pmm.h>
#include <wynland/boot_info.h>

extern uint8_t __kernel_end[];
extern void *memset(void *s, int c, size_t n);
extern void *memcpy(void *dest, const void *src, size_t n);
extern void serial_write_string(const char *str);

/* Snapshot of the boot-time PML4 (kernel code + all identity-mapped RAM +
   framebuffer). Taken once, right after vmm_init() finishes building it and
   before any user process exists -- see vmm_init() below. Every new
   process's PML4 aliases these same top-level entries by pointer so kernel
   code/data/MMIO stay reachable after a CR3 switch into a process. */
static PageTable *g_kernel_pml4 = NULL;

/* Helper to allocate and zero a new page table */
static PageTable *vmm_alloc_table(void)
{
    void *phys = pmm_alloc_page();
    if (!phys) {
        serial_write_string("VMM Error: Out of physical memory for page table!\r\n");
        return NULL;
    }
    memset(phys, 0, PAGE_SIZE);
    return (PageTable *)phys;
}

PageTable *vmm_get_kernel_pml4(void)
{
    return g_kernel_pml4;
}

PageTable *vmm_new_process_pml4(void)
{
    PageTable *pml4 = vmm_alloc_table();
    if (!pml4) return NULL;

    /* Alias every present top-level entry from the kernel PML4 by value --
       this shares the same physical PDPT chains (cheap, no deep copy), and
       is safe because ring-3 access is gated by the PAGE_USER bit at the
       leaf PT entry, not by presence in this table; the kernel's own
       identity-map calls never set PAGE_USER (see vmm_init()). Looping all
       512 entries rather than hardcoding a single index keeps this correct
       even if RAM/framebuffer placement ever grows past what one entry
       covers. */
    for (int i = 0; i < 512; i++) {
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
       kernel/RAM/framebuffer identity-map region every process's PML4
       aliases from vmm_new_process_pml4() (that region is never PAGE_USER,
       see that function's own comment) -- no special-casing needed to avoid
       re-copying it. */
    for (uint64_t i4 = 0; i4 < 512; i4++) {
        if (!(parent_pml4->entries[i4] & PAGE_PRESENT)) continue;
        PageTable *pdpt = (PageTable *)(uintptr_t)(parent_pml4->entries[i4] & PAGE_ADDR_MASK);

        for (uint64_t i3 = 0; i3 < 512; i3++) {
            if (!(pdpt->entries[i3] & PAGE_PRESENT)) continue;
            if (pdpt->entries[i3] & PAGE_PS) continue; /* 1GB huge page -- never produced for user mappings by vmm_map_page, skip defensively */
            PageTable *pd = (PageTable *)(uintptr_t)(pdpt->entries[i3] & PAGE_ADDR_MASK);

            for (uint64_t i2 = 0; i2 < 512; i2++) {
                if (!(pd->entries[i2] & PAGE_PRESENT)) continue;
                if (pd->entries[i2] & PAGE_PS) continue; /* 2MB huge page -- same as above */
                PageTable *pt = (PageTable *)(uintptr_t)(pd->entries[i2] & PAGE_ADDR_MASK);

                for (uint64_t i1 = 0; i1 < 512; i1++) {
                    uint64_t pte = pt->entries[i1];
                    if (!(pte & PAGE_PRESENT)) continue;
                    if (!(pte & PAGE_USER)) continue;

                    uint64_t phys  = pte & PAGE_ADDR_MASK;
                    uint64_t flags = (pte & 0xFFFULL) & ~PAGE_PRESENT; /* vmm_map_page ORs PRESENT in itself */

                    uint64_t virt = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);
                    if (i4 & 0x100) virt |= 0xFFFF000000000000ULL; /* canonical sign-extend, defensive -- this OS's own user addresses never actually reach the upper half today */

                    void *new_phys = pmm_alloc_page();
                    if (!new_phys) {
                        serial_write_string("vmm_clone_user_pages: out of physical memory\r\n");
                        return false;
                    }
                    memcpy(new_phys, (void *)(uintptr_t)phys, PAGE_SIZE);
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
    for (uint64_t i4 = 0; i4 < 512; i4++) {
        if (!(parent_pml4->entries[i4] & PAGE_PRESENT)) continue;
        PageTable *pdpt = (PageTable *)(uintptr_t)(parent_pml4->entries[i4] & PAGE_ADDR_MASK);

        for (uint64_t i3 = 0; i3 < 512; i3++) {
            if (!(pdpt->entries[i3] & PAGE_PRESENT)) continue;
            if (pdpt->entries[i3] & PAGE_PS) continue;
            PageTable *pd = (PageTable *)(uintptr_t)(pdpt->entries[i3] & PAGE_ADDR_MASK);

            for (uint64_t i2 = 0; i2 < 512; i2++) {
                if (!(pd->entries[i2] & PAGE_PRESENT)) continue;
                if (pd->entries[i2] & PAGE_PS) continue;
                PageTable *pt = (PageTable *)(uintptr_t)(pd->entries[i2] & PAGE_ADDR_MASK);

                for (uint64_t i1 = 0; i1 < 512; i1++) {
                    uint64_t pte = pt->entries[i1];
                    if (!(pte & PAGE_PRESENT)) continue;
                    if (!(pte & PAGE_USER)) continue;

                    uint64_t phys  = pte & PAGE_ADDR_MASK;
                    uint64_t flags = (pte & 0xFFFULL) & ~PAGE_PRESENT;

                    uint64_t virt = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);
                    if (i4 & 0x100) virt |= 0xFFFF000000000000ULL;

                    if (pte & PAGE_SHARED_MAP) {
                        /* MAP_SHARED / device memory: the child sees the
                           same frame, writable as before, never COW. Not
                           refcounted by the pmm -- its owner object
                           (SHM segment, DRM BO, ...) decides its lifetime. */
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

                    vmm_map_page(child_pml4, virt, phys, new_flags);
                }
            }
        }
    }
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
    for (uint64_t i4 = 0; i4 < 512; i4++) {
        uint64_t pml4_entry = pml4->entries[i4];
        if (!(pml4_entry & PAGE_PRESENT)) continue;
        if (kernel_pml4 && pml4_entry == kernel_pml4->entries[i4]) continue; /* shared -- never touch */

        PageTable *pdpt = (PageTable *)(uintptr_t)(pml4_entry & PAGE_ADDR_MASK);

        for (uint64_t i3 = 0; i3 < 512; i3++) {
            if (!(pdpt->entries[i3] & PAGE_PRESENT)) continue;
            if (pdpt->entries[i3] & PAGE_PS) continue; /* 1GB huge page -- never produced for a private branch, skip defensively */
            PageTable *pd = (PageTable *)(uintptr_t)(pdpt->entries[i3] & PAGE_ADDR_MASK);

            for (uint64_t i2 = 0; i2 < 512; i2++) {
                if (!(pd->entries[i2] & PAGE_PRESENT)) continue;
                if (pd->entries[i2] & PAGE_PS) continue; /* 2MB huge page -- same as above */
                PageTable *pt = (PageTable *)(uintptr_t)(pd->entries[i2] & PAGE_ADDR_MASK);

                for (uint64_t i1 = 0; i1 < 512; i1++) {
                    uint64_t pte = pt->entries[i1];
                    if (!(pte & PAGE_PRESENT)) continue;
                    if (pte & PAGE_SHARED_MAP) continue; /* owned by its SHM/BO/fb object */
                    /* Refcount-aware: a leaf still COW-shared with a sibling
                       process (fork()'d, never written since) just drops
                       this owner's share here instead of freeing outright. */
                    pmm_free_page((void *)(uintptr_t)(pte & PAGE_ADDR_MASK));
                }
                pmm_free_page(pt); /* privately owned -- this whole branch isn't shared */
            }
            pmm_free_page(pd);
        }
        pmm_free_page(pdpt);
        pml4->entries[i4] = 0;
    }
}

void vmm_destroy_process_pml4(PageTable *pml4)
{
    if (!pml4) return;
    vmm_free_user_mappings(pml4);
    pmm_free_page(pml4);
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
        *pml4_entry = (uint64_t)(uintptr_t)pdpt | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    } else {
        pdpt = (PageTable *)(uintptr_t)(*pml4_entry & PAGE_ADDR_MASK);
    }

    /* 2. Traverse PDPT -> PD */
    PageTableEntry *pdpt_entry = &pdpt->entries[pdpt_idx];
    PageTable *pd = NULL;
    if (!(*pdpt_entry & PAGE_PRESENT)) {
        pd = vmm_alloc_table();
        *pdpt_entry = (uint64_t)(uintptr_t)pd | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    } else {
        pd = (PageTable *)(uintptr_t)(*pdpt_entry & PAGE_ADDR_MASK);
    }

    /* 3. Traverse PD -> PT */
    PageTableEntry *pd_entry = &pd->entries[pd_idx];
    PageTable *pt = NULL;
    if (!(*pd_entry & PAGE_PRESENT)) {
        pt = vmm_alloc_table();
        *pd_entry = (uint64_t)(uintptr_t)pt | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    } else {
        pt = (PageTable *)(uintptr_t)(*pd_entry & PAGE_ADDR_MASK);
    }

    /* 4. Map the physical address in the Page Table */
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

    /* 5. Invalidate TLB for this virtual address */
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");

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
    
    PageTable *pdpt = (PageTable *)(uintptr_t)(*pml4_entry & PAGE_ADDR_MASK);
    PageTableEntry *pdpt_entry = &pdpt->entries[pdpt_idx];
    if (!(*pdpt_entry & PAGE_PRESENT)) goto cleanup;

    PageTable *pd = (PageTable *)(uintptr_t)(*pdpt_entry & PAGE_ADDR_MASK);
    PageTableEntry *pd_entry = &pd->entries[pd_idx];
    if (!(*pd_entry & PAGE_PRESENT)) goto cleanup;

    PageTable *pt = (PageTable *)(uintptr_t)(*pd_entry & PAGE_ADDR_MASK);
    
    /* Clear entry */
    pt->entries[pt_idx] = 0;

    /* Invalidate TLB */
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");

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

    PageTable *pdpt = (PageTable *)(uintptr_t)(*pml4_entry & PAGE_ADDR_MASK);
    PageTableEntry *pdpt_entry = &pdpt->entries[pdpt_idx];
    if (!(*pdpt_entry & PAGE_PRESENT)) goto cleanup;

    PageTable *pd = (PageTable *)(uintptr_t)(*pdpt_entry & PAGE_ADDR_MASK);
    PageTableEntry *pd_entry = &pd->entries[pd_idx];
    if (!(*pd_entry & PAGE_PRESENT)) goto cleanup;

    PageTable *pt = (PageTable *)(uintptr_t)(*pd_entry & PAGE_ADDR_MASK);
    PageTableEntry *pt_entry = &pt->entries[pt_idx];
    if (!(*pt_entry & PAGE_PRESENT)) goto cleanup;

    uint64_t phys = *pt_entry & PAGE_ADDR_MASK;
    *pt_entry = phys | flags | PAGE_PRESENT;
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
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

    PageTable *pdpt = (PageTable *)(uintptr_t)(*pml4_entry & PAGE_ADDR_MASK);
    PageTableEntry *pdpt_entry = &pdpt->entries[pdpt_idx];
    if (!(*pdpt_entry & PAGE_PRESENT)) return 0;

    PageTable *pd = (PageTable *)(uintptr_t)(*pdpt_entry & PAGE_ADDR_MASK);
    PageTableEntry *pd_entry = &pd->entries[pd_idx];
    if (!(*pd_entry & PAGE_PRESENT)) return 0;

    PageTable *pt = (PageTable *)(uintptr_t)(*pd_entry & PAGE_ADDR_MASK);
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

    memcpy(new_phys, (void *)(uintptr_t)phys, PAGE_SIZE);
    vmm_map_page(pml4, page_addr, (uint64_t)(uintptr_t)new_phys, (flags & ~PAGE_COW) | PAGE_WRITE);
    pmm_free_page((void *)(uintptr_t)phys); /* drop this owner's share of the old, still-shared frame */
    return true;
}

uint64_t vmm_get_pte(PageTable *pml4, uint64_t virt)
{
    PageTableEntry e = pml4->entries[PML4_INDEX(virt)];
    if (!(e & PAGE_PRESENT)) return 0;
    PageTable *pdpt = (PageTable *)(uintptr_t)(e & PAGE_ADDR_MASK);
    e = pdpt->entries[PDPT_INDEX(virt)];
    if (!(e & PAGE_PRESENT) || (e & PAGE_PS)) return 0;
    PageTable *pd = (PageTable *)(uintptr_t)(e & PAGE_ADDR_MASK);
    e = pd->entries[PD_INDEX(virt)];
    if (!(e & PAGE_PRESENT) || (e & PAGE_PS)) return 0;
    PageTable *pt = (PageTable *)(uintptr_t)(e & PAGE_ADDR_MASK);
    e = pt->entries[PT_INDEX(virt)];
    return (e & PAGE_PRESENT) ? e : 0;
}

uint64_t vmm_get_phys(PageTable *pml4, uint64_t virt)
{
    uint64_t pml4_idx = PML4_INDEX(virt);
    uint64_t pdpt_idx = PDPT_INDEX(virt);
    uint64_t pd_idx   = PD_INDEX(virt);
    uint64_t pt_idx   = PT_INDEX(virt);

    PageTableEntry *pml4_entry = &pml4->entries[pml4_idx];
    if (!(*pml4_entry & PAGE_PRESENT)) return 0;
    
    PageTable *pdpt = (PageTable *)(uintptr_t)(*pml4_entry & PAGE_ADDR_MASK);
    PageTableEntry *pdpt_entry = &pdpt->entries[pdpt_idx];
    if (!(*pdpt_entry & PAGE_PRESENT)) return 0;
    if (*pdpt_entry & PAGE_PS) {
        /* 1GB huge page */
        return (*pdpt_entry & PAGE_ADDR_MASK) | (virt & 0x3FFFFFFFULL);
    }

    PageTable *pd = (PageTable *)(uintptr_t)(*pdpt_entry & PAGE_ADDR_MASK);
    PageTableEntry *pd_entry = &pd->entries[pd_idx];
    if (!(*pd_entry & PAGE_PRESENT)) return 0;
    if (*pd_entry & PAGE_PS) {
        /* 2MB huge page */
        return (*pd_entry & PAGE_ADDR_MASK) | (virt & 0x1FFFFFULL);
    }

    PageTable *pt = (PageTable *)(uintptr_t)(*pd_entry & PAGE_ADDR_MASK);
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

    PageTable *pdpt = (PageTable *)(uintptr_t)(*pml4_entry & PAGE_ADDR_MASK);
    PageTableEntry *pdpt_entry = &pdpt->entries[pdpt_idx];
    if (!(*pdpt_entry & PAGE_PRESENT)) return false;
    if (*pdpt_entry & PAGE_PS) return (*pdpt_entry & PAGE_USER) != 0;

    PageTable *pd = (PageTable *)(uintptr_t)(*pdpt_entry & PAGE_ADDR_MASK);
    PageTableEntry *pd_entry = &pd->entries[pd_idx];
    if (!(*pd_entry & PAGE_PRESENT)) return false;
    if (*pd_entry & PAGE_PS) return (*pd_entry & PAGE_USER) != 0;

    PageTable *pt = (PageTable *)(uintptr_t)(*pd_entry & PAGE_ADDR_MASK);
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

    /* 1. Identity map the kernel code & data [0x100000, __kernel_end] */
    uint64_t kernel_start = 0x100000;
    uint64_t kernel_end = ((uint64_t)__kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t addr = kernel_start; addr < kernel_end; addr += PAGE_SIZE) {
        vmm_map_page(pml4, addr, addr, PAGE_WRITE);
    }

    /* 2. Identity map all memory regions from UEFI memory map */
    MemoryRegion *regions = (MemoryRegion *)(uintptr_t)boot_info->mmap_addr;
    uint32_t region_count = boot_info->mmap_entries;

    for (uint32_t i = 0; i < region_count; i++) {
        /* Skip reserved and bad memory to avoid mapping unbacked/MMIO holes */
        if (regions[i].type == MEMORY_RESERVED || regions[i].type == MEMORY_BAD) {
            continue;
        }

        uint64_t base = regions[i].base;
        uint64_t size = regions[i].size;

        uint64_t flags = PAGE_WRITE;
        if (regions[i].type != MEMORY_USABLE) {
            flags |= PAGE_NX; /* Mark non-usable memory as No-Execute */
        }

        for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE) {
            uint64_t addr = base + offset;
            /* Skip only the pages that fall within the kernel boundaries (already mapped) */
            if (addr >= kernel_start && addr < kernel_end) {
                continue;
            }
            vmm_map_page(pml4, addr, addr, flags);
        }
    }

    /* Configure PAT4 as Write-Combining (01h) in PAT MSR 0x277 */
    uint64_t pat = read_msr(0x277);
    pat &= ~(0xFFULL << 32); /* Clear PAT4 (bits 32..39) */
    pat |= (0x01ULL << 32);  /* Set PAT4 to WC (01h) */
    write_msr(0x277, pat);

    /* 3. Identity map the Graphics Output Protocol (GOP) Framebuffer */
    uint64_t fb_start = boot_info->fb_addr;
    uint64_t fb_size = boot_info->fb_pitch * boot_info->fb_height;
    fb_size = (fb_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t offset = 0; offset < fb_size; offset += PAGE_SIZE) {
        vmm_map_page(pml4, fb_start + offset, fb_start + offset, PAGE_WRITE | PAGE_NX | PAGE_PAT);
    }

    /* Snapshot this as the kernel PML4 -- at this exact point it's
       guaranteed to contain only the kernel/RAM/framebuffer identity map
       and nothing else, since no user process has run yet. New processes
       (see vmm_new_process_pml4()) alias these entries by pointer. */
    g_kernel_pml4 = pml4;

    /* 4. Switch to our new PML4 page table by loading it into CR3 */
    uint64_t pml4_phys = (uint64_t)(uintptr_t)pml4;
    __asm__ volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");

    serial_write_string("VMM: Switched to new page tables successfully!\r\n");
}

PageTable *vmm_get_current_pml4(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (PageTable *)(uintptr_t)cr3;
}

void vmm_map_mmio(uint64_t phys_addr, uint64_t size)
{
    PageTable *pml4 = vmm_get_current_pml4();
    uint64_t start = phys_addr & ~(PAGE_SIZE - 1);
    uint64_t end = (phys_addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
        vmm_map_page(pml4, addr, addr, PAGE_WRITE | PAGE_NX | PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH);
    }
}

