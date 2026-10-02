/*
 * WynlandOS - Safe user-memory access helpers
 * ============================================================
 * See include/wynland/usercopy.h for the rationale. Every function
 * here operates on the CURRENT process's page tables (CR3 doesn't
 * change across a syscall -- kernel/syscall.c already relies on that
 * fact via vmm_get_current_pml4()), so none of them take a pml4
 * argument except user_range_valid() itself, which a caller validating
 * a non-current address space (there are none today) could still use
 * directly.
 */
#include <wynland/usercopy.h>
#include <wynland/vmm.h>
#include <wynland/pmm.h>
#include <wynland/sched.h>
#include <wynland/process.h>
#include <wynland/vma.h>

extern void *memcpy(void *dest, const void *src, size_t n);

/* Highest exclusive address this kernel ever treats as a legitimate
   user address: bit 47 and above must be zero (the canonical-low-half
   requirement), which also happens to exclude PML4 index 0 only when
   combined with the PAGE_USER check below -- the shared kernel/RAM/
   framebuffer identity map (see vmm_new_process_pml4()) lives inside
   this same bound but is never PAGE_USER, so it's rejected on that
   basis instead. */
#define USER_ADDR_LIMIT 0x0000800000000000ULL

bool user_range_valid(PageTable *pml4, uint64_t addr, uint64_t len, uint32_t required_access)
{
    if (len == 0) return true;

    uint64_t end = addr + len; /* exclusive */
    if (end < addr) return false;              /* addr+len overflow */
    if (end > USER_ADDR_LIMIT) return false;    /* non-canonical / kernel-half */

    uint64_t start_page = addr & ~(PAGE_SIZE - 1);
    uint64_t end_page = (end - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t page = start_page; ; page += PAGE_SIZE) {
        uint64_t flags = vmm_get_page_flags(pml4, page);
        /* a lazy page nobody touched yet: give it its frame now (a read()
           into a fresh malloc'ed buffer must not fail with EFAULT) */
        if (!(flags & PAGE_PRESENT) && pml4 == vmm_get_current_pml4()) {
            Thread *t = sched_current();
            uint32_t acc = (required_access & UACCESS_WRITE) ? VMA_PROT_WRITE : VMA_PROT_READ;
            if (t && t->proc && vma_fault_in(t->proc, page, acc))
                flags = vmm_get_page_flags(pml4, page);
        }
        if (!(flags & PAGE_PRESENT) || !(flags & PAGE_USER)) return false;
        if ((required_access & UACCESS_WRITE) && !(flags & PAGE_WRITE) && !(flags & PAGE_COW)) {
            return false; /* read-only page (e.g. mprotect(PROT_READ), .text) */
        }
        if (page == end_page) break;
    }
    return true;
}

bool user_check_read(uint64_t addr, uint64_t len)
{
    return user_range_valid(vmm_get_current_pml4(), addr, len, UACCESS_READ);
}

bool user_prepare_write(uint64_t addr, uint64_t len)
{
    PageTable *pml4 = vmm_get_current_pml4();
    if (!user_range_valid(pml4, addr, len, UACCESS_WRITE)) return false;
    if (len == 0) return true;

    uint64_t start_page = addr & ~(PAGE_SIZE - 1);
    uint64_t end_page = (addr + len - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t page = start_page; ; page += PAGE_SIZE) {
        if (!vmm_resolve_cow_page(pml4, page)) return false;
        if (page == end_page) break;
    }
    return true;
}

int copy_from_user(void *kdst, const void *usrc, uint64_t len)
{
    PageTable *pml4 = vmm_get_current_pml4();
    if (!user_range_valid(pml4, (uint64_t)(uintptr_t)usrc, len, UACCESS_READ)) {
        return -14; /* -EFAULT */
    }
    if (len > 0) memcpy(kdst, usrc, len);
    return 0;
}

int copy_to_user(void *udst, const void *ksrc, uint64_t len)
{
    /* user_prepare_write() validates the range AND resolves every COW page
       in it -- a syscall runs at CPL0 with the user's own CR3 still
       loaded, so a raw write to a still-shared COW frame would either
       silently corrupt the sibling process (if CR0.WP is clear) or take a
       kernel-mode #PF that the page-fault handler's ring-3-only COW fast
       path (kernel/idt.c) does NOT resolve. */
    if (!user_prepare_write((uint64_t)(uintptr_t)udst, len)) {
        return -14; /* -EFAULT */
    }
    if (len > 0) memcpy(udst, ksrc, len);
    return 0;
}

int64_t strncpy_from_user(char *dst, const void *usrc, uint64_t maxlen)
{
    if (maxlen == 0) return -22; /* -EINVAL */

    PageTable *pml4 = vmm_get_current_pml4();
    const char *src = (const char *)usrc;
    uint64_t i = 0;
    uint64_t last_page = ~0ULL;

    while (i < maxlen - 1) {
        uint64_t addr = (uint64_t)(uintptr_t)(src + i);
        uint64_t page = addr & ~(PAGE_SIZE - 1);
        if (page != last_page) {
            if (!user_range_valid(pml4, page, PAGE_SIZE, UACCESS_READ)) {
                dst[i] = '\0';
                return -14; /* -EFAULT */
            }
            last_page = page;
        }
        char c = src[i];
        dst[i] = c;
        if (c == '\0') return (int64_t)i;
        i++;
    }
    dst[maxlen - 1] = '\0';
    return -36; /* -ENAMETOOLONG */
}
