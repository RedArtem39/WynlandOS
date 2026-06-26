/*
 * WynlandOS - Kernel Heap Allocator Implementation
 */

#include <wynland/heap.h>
#include <wynland/pmm.h>
#include <wynland/vmm.h>

extern void serial_write_string(const char *str);
extern void uint_to_hex(uint64_t val, char *buf);

typedef struct HeapHeader {
    size_t size;             /* Size of the data block in bytes */
    bool is_free;            /* Is this block free? */
    struct HeapHeader *next; /* Next block in the list */
} HeapHeader;

static HeapHeader *heap_first = NULL;
static uint64_t heap_end_addr = HEAP_START;
static uint64_t heap_max_limit = HEAP_START + (HEAP_MAX_PAGES * PAGE_SIZE);

/* Alignment helper (aligns to 16 bytes) */
static inline size_t align_up(size_t val)
{
    return (val + 15) & ~15U;
}

void heap_init(void)
{
    serial_write_string("Heap: Initializing kernel heap...\r\n");

    PageTable *pml4 = vmm_get_current_pml4();

    /* Allocate initial pages for the heap */
    for (uint64_t i = 0; i < HEAP_INITIAL_PAGES; i++) {
        void *phys = pmm_alloc_page();
        if (!phys) {
            serial_write_string("Heap Error: Out of physical memory during heap_init!\r\n");
            return;
        }
        uint64_t virt = HEAP_START + i * PAGE_SIZE;
        vmm_map_page(pml4, virt, (uint64_t)(uintptr_t)phys, PAGE_WRITE | PAGE_NX);
    }

    heap_first = (HeapHeader *)HEAP_START;
    heap_first->size = (HEAP_INITIAL_PAGES * PAGE_SIZE) - sizeof(HeapHeader);
    heap_first->is_free = true;
    heap_first->next = NULL;

    heap_end_addr = HEAP_START + (HEAP_INITIAL_PAGES * PAGE_SIZE);

    serial_write_string("Heap: Initialized successfully with ");
    char buf[32];
    uint_to_hex(HEAP_INITIAL_PAGES * PAGE_SIZE, buf);
    serial_write_string(buf);
    serial_write_string(" bytes.\r\n");
}

static bool heap_grow(size_t size_needed)
{
    PageTable *pml4 = vmm_get_current_pml4();

    /* Determine how many pages we need to allocate */
    size_t total_needed = size_needed + sizeof(HeapHeader);
    size_t pages_needed = (total_needed + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t new_end = heap_end_addr + (pages_needed * PAGE_SIZE);

    if (new_end > heap_max_limit) {
        serial_write_string("Heap Error: Cannot grow heap, limit reached!\r\n");
        return false;
    }

    /* Allocate and map new physical pages */
    for (size_t i = 0; i < pages_needed; i++) {
        void *phys = pmm_alloc_page();
        if (!phys) {
            serial_write_string("Heap Error: Out of physical memory during heap_grow!\r\n");
            return false;
        }
        uint64_t virt = heap_end_addr + i * PAGE_SIZE;
        vmm_map_page(pml4, virt, (uint64_t)(uintptr_t)phys, PAGE_WRITE | PAGE_NX);
    }

    /* Create the new free block representing the grown space */
    HeapHeader *new_block = (HeapHeader *)heap_end_addr;
    new_block->size = (pages_needed * PAGE_SIZE) - sizeof(HeapHeader);
    new_block->is_free = true;
    new_block->next = NULL;

    /* Append to the end of the block list */
    HeapHeader *curr = heap_first;
    while (curr->next != NULL) {
        curr = curr->next;
    }
    curr->next = new_block;

    heap_end_addr = new_end;

    /* Coalesce immediately to merge the new block if the previous one was free */
    kfree(NULL); /* Passing NULL triggers coalescing only */

    return true;
}

void *kmalloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    size_t aligned_size = align_up(size);

    /* Search for first free block that fits */
    HeapHeader *curr = heap_first;
    while (curr != NULL) {
        if (curr->is_free && curr->size >= aligned_size) {
            break;
        }
        curr = curr->next;
    }

    /* If no block is found, grow the heap */
    if (curr == NULL) {
        if (!heap_grow(aligned_size)) {
            return NULL; /* Out of memory */
        }
        /* Search again from the start (should succeed now) */
        curr = heap_first;
        while (curr != NULL) {
            if (curr->is_free && curr->size >= aligned_size) {
                break;
            }
            curr = curr->next;
        }
        if (curr == NULL) {
            return NULL;
        }
    }

    /* If we have enough remaining space in the block, split it */
    if (curr->size >= aligned_size + sizeof(HeapHeader) + 16) {
        HeapHeader *next_block = (HeapHeader *)((uintptr_t)curr + sizeof(HeapHeader) + aligned_size);
        next_block->size = curr->size - aligned_size - sizeof(HeapHeader);
        next_block->is_free = true;
        next_block->next = curr->next;

        curr->size = aligned_size;
        curr->next = next_block;
    }

    curr->is_free = false;
    return (void *)((uintptr_t)curr + sizeof(HeapHeader));
}

void kfree(void *ptr)
{
    if (ptr != NULL) {
        HeapHeader *block = (HeapHeader *)((uintptr_t)ptr - sizeof(HeapHeader));
        block->is_free = true;
    }

    /* Coalesce contiguous free blocks */
    HeapHeader *curr = heap_first;
    while (curr != NULL && curr->next != NULL) {
        if (curr->is_free && curr->next->is_free) {
            curr->size += sizeof(HeapHeader) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

size_t heap_get_used_memory(void)
{
    size_t used = 0;
    HeapHeader *curr = heap_first;
    while (curr != NULL) {
        if (!curr->is_free) {
            used += curr->size + sizeof(HeapHeader);
        }
        curr = curr->next;
    }
    return used;
}

size_t heap_get_free_memory(void)
{
    size_t free_mem = 0;
    HeapHeader *curr = heap_first;
    while (curr != NULL) {
        if (curr->is_free) {
            free_mem += curr->size;
        }
        curr = curr->next;
    }
    return free_mem;
}
