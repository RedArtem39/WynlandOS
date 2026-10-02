/*
 * WynlandOS - Kernel Heap Allocator Implementation
 */

#include <wynland/heap.h>
#include <wynland/pmm.h>
#include <wynland/vmm.h>

extern void serial_write_string(const char *str);
extern void uint_to_hex(uint64_t val, char *buf);

/* Blocks tile the heap in address order: next/prev are also the
   physical neighbours, so freeing merges with them directly. kfree() used
   to walk the whole list with interrupts off on every call, and could not
   tell a double free from a first one. */
typedef struct HeapHeader {
    size_t size;             /* Size of the data block in bytes */
    struct HeapHeader *next; /* following block (higher address) */
    struct HeapHeader *prev; /* preceding block (lower address) */
    uint32_t magic;          /* HEAP_MAGIC_USED / HEAP_MAGIC_FREE */
    bool is_free;            /* Is this block free? */
} HeapHeader;

#define HEAP_MAGIC_USED 0x4B4D5553u   /* "KMUS" */
#define HEAP_MAGIC_FREE 0x4B4D4652u   /* "KMFR" */

/* b absorbs the free block right after it */
static void merge_next(HeapHeader *b)
{
    HeapHeader *n = b->next;
    b->size += sizeof(HeapHeader) + n->size;
    b->next = n->next;
    if (b->next) b->next->prev = b;
    n->magic = 0;
}

static HeapHeader *heap_first = NULL;
uint64_t heap_end_addr = HEAP_START;
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
    heap_first->magic = HEAP_MAGIC_FREE;
    heap_first->next = NULL;
    heap_first->prev = NULL;

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
    new_block->magic = HEAP_MAGIC_FREE;
    new_block->next = NULL;

    /* Append to the end of the block list */
    HeapHeader *curr = heap_first;
    while (curr->next != NULL) {
        curr = curr->next;
    }
    curr->next = new_block;
    new_block->prev = curr;

    heap_end_addr = new_end;

    /* merge with the last block if that one is free */
    if (curr->is_free) merge_next(curr);

    return true;
}

void *kmalloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

    void *result = NULL;
    size_t aligned_size = align_up(size);

    /*
    serial_write_string("kmalloc: size=0x");
    char buf[32];
    uint_to_hex((uint64_t)size, buf); serial_write_string(buf);
    serial_write_string(" first=0x");
    uint_to_hex((uint64_t)(uintptr_t)heap_first, buf); serial_write_string(buf);
    serial_write_string(" end=0x");
    uint_to_hex(heap_end_addr, buf); serial_write_string(buf);
    serial_write_string("\r\n");
    */

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
            goto cleanup; /* Out of memory */
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
            goto cleanup;
        }
    }

    /* If we have enough remaining space in the block, split it cleanly */
    if (curr->size >= aligned_size + sizeof(HeapHeader) + 32) {
        HeapHeader *next_block = (HeapHeader *)((uintptr_t)curr + sizeof(HeapHeader) + aligned_size);
        next_block->size = curr->size - aligned_size - sizeof(HeapHeader);
        next_block->is_free = true;
        next_block->magic = HEAP_MAGIC_FREE;
        next_block->next = curr->next;
        next_block->prev = curr;
        if (next_block->next) next_block->next->prev = next_block;

        curr->size = aligned_size;
        curr->next = next_block;
    }

    curr->is_free = false;
    curr->magic = HEAP_MAGIC_USED;
    result = (void *)((uintptr_t)curr + sizeof(HeapHeader));

cleanup:
    if (rflags & 0x200) {
        __asm__ volatile("sti");
    }
    return result;
}

void kfree(void *ptr)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

    if (ptr != NULL) {
        HeapHeader *block = (HeapHeader *)((uintptr_t)ptr - sizeof(HeapHeader));
        uint64_t a = (uint64_t)(uintptr_t)block;
        bool in_heap = a >= HEAP_START && a < heap_end_addr && !((uintptr_t)ptr & 15);
        if (!in_heap || block->magic != HEAP_MAGIC_USED || block->is_free) {
            /* a double free, or a pointer kmalloc() never returned:
               ignore it rather than corrupt the heap, and say so */
            char buf[32];
            serial_write_string(in_heap && block->magic == HEAP_MAGIC_FREE ? "Heap: double kfree " : "Heap: bad kfree ");
            uint_to_hex((uint64_t)(uintptr_t)ptr, buf);
            serial_write_string(buf);
            serial_write_string("\r\n");
        } else {
            block->is_free = true;
            block->magic = HEAP_MAGIC_FREE;
            /* merge with the neighbours only: O(1) */
            if (block->next && block->next->is_free) merge_next(block);
            if (block->prev && block->prev->is_free) merge_next(block->prev);
        }
    }

    if (rflags & 0x200) {
        __asm__ volatile("sti");
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

size_t heap_get_block_size(void *ptr)
{
    if (ptr == NULL) {
        return 0;
    }
    HeapHeader *block = (HeapHeader *)((uintptr_t)ptr - sizeof(HeapHeader));
    return block->size;
}
