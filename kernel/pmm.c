/*
 * WynlandOS - Physical Memory Manager (PMM) Implementation
 */

#include <wynland/pmm.h>
#include <wynland/boot_info.h>

extern uint8_t __kernel_end[];
extern void serial_write_string(const char *str);

static uint8_t  *bitmap = NULL;
static uint8_t  *refcount = NULL;  /* one byte per page frame; 0 = untracked/exclusive owner path via bitmap alone */
static uint64_t total_pages = 0;
static uint64_t free_pages = 0;
static uint64_t total_mem_size = 0;
static uint64_t total_usable_memory = 0;

/* Helpers for bitmap operations */
static inline void bitmap_set(uint64_t page_index)
{
    bitmap[page_index / 8] |= (1 << (page_index % 8));
}

static inline void bitmap_clear(uint64_t page_index)
{
    bitmap[page_index / 8] &= ~(1 << (page_index % 8));
}

static inline bool bitmap_test(uint64_t page_index)
{
    return (bitmap[page_index / 8] & (1 << (page_index % 8))) != 0;
}

void pmm_init(BootInfo *boot_info)
{
    MemoryRegion *regions = (MemoryRegion *)(uintptr_t)boot_info->mmap_addr;
    uint32_t region_count = boot_info->mmap_entries;

    /* 1. Find the highest physical memory address to size our bitmap and sum usable memory */
    uint64_t highest_address = 0;
    total_usable_memory = 0;
    for (uint32_t i = 0; i < region_count; i++) {
        uint64_t limit = regions[i].base + regions[i].size;
        if (limit > highest_address) {
            highest_address = limit;
        }
        if (regions[i].type == MEMORY_USABLE) {
            total_usable_memory += regions[i].size;
        }
    }

    total_mem_size = highest_address;
    total_pages = highest_address / PAGE_SIZE;
    uint64_t bitmap_size = (total_pages + 7) / 8;

    /* 2. Find a free memory region large enough to store the bitmap */
    uint64_t bitmap_phys_addr = 0;
    for (uint32_t i = 0; i < region_count; i++) {
        if (regions[i].type == MEMORY_USABLE && regions[i].size >= bitmap_size) {
            /* Keep it aligned above the kernel region to be safe */
            if (regions[i].base >= 0x100000) {
                bitmap_phys_addr = regions[i].base;
                break;
            }
        }
    }

    /* Fallback if no safe high memory is found, try any usable region */
    if (bitmap_phys_addr == 0) {
        for (uint32_t i = 0; i < region_count; i++) {
            if (regions[i].type == MEMORY_USABLE && regions[i].size >= bitmap_size) {
                bitmap_phys_addr = regions[i].base;
                break;
            }
        }
    }

    bitmap = (uint8_t *)(uintptr_t)bitmap_phys_addr;

    /* 3. Mark the entire memory space as used/reserved by default (bits = 1) */
    memset(bitmap, 0xFF, bitmap_size);
    free_pages = 0;

    /* 4. Mark all MEMORY_USABLE regions as free (bits = 0) */
    for (uint32_t i = 0; i < region_count; i++) {
        if (regions[i].type == MEMORY_USABLE) {
            uint64_t start_page = regions[i].base / PAGE_SIZE;
            uint64_t page_count = regions[i].size / PAGE_SIZE;

            for (uint64_t p = 0; p < page_count; p++) {
                uint64_t idx = start_page + p;
                if (idx < total_pages) {
                    bitmap_clear(idx);
                    free_pages++;
                }
            }
        }
    }

    /* 5. Manually reserve the first 1MB of memory (contains BIOS/UEFI tables, stack, etc.) */
    uint64_t first_mb_pages = 0x100000 / PAGE_SIZE;
    for (uint64_t i = 0; i < first_mb_pages; i++) {
        if (i < total_pages) {
            if (!bitmap_test(i)) {
                bitmap_set(i);
                free_pages--;
            }
        }
    }

    /* 6. Manually reserve the kernel memory space [0x100000, __kernel_end] */
    uint64_t kernel_start_page = 0x100000 / PAGE_SIZE;
    uint64_t kernel_end_phys = ((uint64_t)__kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t kernel_end_page = kernel_end_phys / PAGE_SIZE;

    for (uint64_t i = kernel_start_page; i < kernel_end_page; i++) {
        if (i < total_pages) {
            if (!bitmap_test(i)) {
                bitmap_set(i);
                free_pages--;
            }
        }
    }

    /* 7. Manually reserve the memory occupied by the PMM bitmap itself */
    uint64_t bitmap_start_page = bitmap_phys_addr / PAGE_SIZE;
    uint64_t bitmap_end_page = (bitmap_phys_addr + bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = bitmap_start_page; i < bitmap_end_page; i++) {
        if (i < total_pages) {
            if (!bitmap_test(i)) {
                bitmap_set(i);
                free_pages--;
            }
        }
    }

    /* 8. Home the per-frame refcount table (one byte per page, used by
       fork()'s COW sharing -- see vmm_cow_clone_user_pages()) right after
       the bitmap in the same region -- both are tiny (KB-scale) compared to
       any usable region large enough to hold the bitmap in the first place,
       so this avoids a second region search and its edge cases entirely. */
    uint64_t refcount_size = total_pages;
    uint64_t refcount_phys_addr = (bitmap_phys_addr + bitmap_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    refcount = (uint8_t *)(uintptr_t)refcount_phys_addr;
    memset(refcount, 0, refcount_size);

    uint64_t refcount_start_page = refcount_phys_addr / PAGE_SIZE;
    uint64_t refcount_end_page = (refcount_phys_addr + refcount_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = refcount_start_page; i < refcount_end_page; i++) {
        if (i < total_pages) {
            if (!bitmap_test(i)) {
                bitmap_set(i);
                free_pages--;
            }
        }
    }
}

void *pmm_alloc_page(void)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

    void *result = NULL;
    uint64_t bitmap_bytes = (total_pages + 7) / 8;
    for (uint64_t i = 0; i < bitmap_bytes; i++) {
        if (bitmap[i] != 0xFF) { /* At least one free bit */
            for (int bit = 0; bit < 8; bit++) {
                uint64_t idx = i * 8 + bit;
                if (idx >= total_pages) {
                    break;
                }
                if (!bitmap_test(idx)) {
                    bitmap_set(idx);
                    free_pages--;
                    refcount[idx] = 1;
                    result = (void *)(idx * PAGE_SIZE);
                    {
                        extern uint64_t heap_end_addr;
                        static int alias_logged;
                        if (idx * PAGE_SIZE >= 0x10000000ULL && idx * PAGE_SIZE < heap_end_addr && !alias_logged) {
                            extern void serial_write_string(const char *);
                            /* Known gap: the kernel heap's virtual window (HEAP_START..) sits on top of
                               the RAM identity map, so a frame with this physical
                               address is not reachable through its identity address.
                               Not hit yet (under 256 MB in use); say so if it is. */
                            serial_write_string("PMM: WARNING frame aliased by the kernel heap window handed out\r\n");
                            alias_logged = 1;
                        }
                    }
                    goto cleanup;
                }
            }
        }
    }

cleanup:
    if (rflags & 0x200) {
        __asm__ volatile("sti");
    }
    /*
    if (result) {
        serial_write_string("PMM Alloc: ");
        char buf[32];
        extern void uint_to_hex(uint64_t val, char *buf);
        uint_to_hex((uint64_t)(uintptr_t)result, buf);
        serial_write_string(buf);
        serial_write_string("\r\n");
    }
    */
    return result;
}

void *pmm_alloc_contiguous(uint32_t count)
{
    if (count == 0) return NULL;

    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

    void *result = NULL;
    uint32_t run = 0;
    uint64_t start_idx = 0;

    for (uint64_t idx = 0; idx < total_pages; idx++) {
        if (!bitmap_test(idx)) {
            if (run == 0) {
                start_idx = idx;
            }
            run++;
            if (run == count) {
                /* Found contiguous range! Mark all as allocated */
                for (uint64_t j = start_idx; j < start_idx + count; j++) {
                    bitmap_set(j);
                    refcount[j] = 1;
                }
                free_pages -= count;
                result = (void *)(start_idx * PAGE_SIZE);
                goto cleanup;
            }
        } else {
            run = 0;
        }
    }

cleanup:
    if (rflags & 0x200) {
        __asm__ volatile("sti");
    }
    return result;
}

void pmm_free_page(void *addr)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

    uint64_t idx = (uint64_t)addr / PAGE_SIZE;
    if (idx < total_pages) {
        if (bitmap_test(idx)) {
            if (refcount[idx] > 1) {
                /* Still shared (COW) -- drop this owner's reference, keep
                   the frame allocated for the remaining owner(s). */
                refcount[idx]--;
            } else {
                bitmap_clear(idx);
                refcount[idx] = 0;
                free_pages++;
            }
        }
    }

    if (rflags & 0x200) {
        __asm__ volatile("sti");
    }
}

void pmm_page_incref(void *addr)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

    uint64_t idx = (uint64_t)addr / PAGE_SIZE;
    if (idx < total_pages && bitmap_test(idx)) {
        if (refcount[idx] == 255) {
            extern void serial_write_string(const char *);
            serial_write_string("PMM: refcount overflow\r\n");
        }
        refcount[idx]++;
    }

    if (rflags & 0x200) {
        __asm__ volatile("sti");
    }
}

uint32_t pmm_page_refcount(void *addr)
{
    uint64_t idx = (uint64_t)addr / PAGE_SIZE;
    if (idx < total_pages && bitmap_test(idx)) {
        return refcount[idx];
    }
    return 0;
}

uint64_t pmm_get_total_memory(void)
{
    return total_usable_memory;
}

uint64_t pmm_get_free_memory(void)
{
    return free_pages * PAGE_SIZE;
}

uint64_t pmm_get_used_memory(void)
{
    return total_usable_memory - (free_pages * PAGE_SIZE);
}
