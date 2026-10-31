/*
 * WynlandOS - Swap (include/wynland/swap.h)
 *
 * Slots: a reference count each (0 free): a forked child shares its
 * parent's slot until one of them reads it back. Reclaim is a clock over
 * every process's page tables: a page the CPU marked accessed since the
 * last pass loses the mark and stays; one without it goes out. A batch of
 * victims first becomes swap entries and is flushed from every core's TLB
 * -- nobody can change the pages after that -- then is written out and its
 * frames freed. The reclaim thread keeps free RAM between two watermarks;
 * an allocation that finds none at all reclaims directly (vma.c).
 */
#include <wynland/swap.h>
#include <wynland/vmm.h>
#include <wynland/pmm.h>
#include <wynland/heap.h>
#include <wynland/sched.h>
#include <wynland/process.h>
#include <wynland/vma.h>
#include <wynland/kmutex.h>
#include <wynland/smp.h>
#include <wynland/spinlock.h>

extern void serial_write_string(const char *str);
extern void *memcpy(void *dest, const void *src, size_t n);
extern void uint_to_str(uint64_t val, char *buf);
extern bool ahci_read(uint32_t lba, uint32_t count, void *buf);
extern bool ahci_write(uint32_t lba, uint32_t count, const void *buf);
extern KMutex g_fs_mutex;                /* the disk's one command slot (drivers/fs/ext2.c) */
extern uint64_t timer_get_ms(void);

#define SECTORS_PER_SLOT (PAGE_SIZE / 512)
#define PAGE_ACCESSED    (1ULL << 5)
#define BATCH            64

static uint32_t g_lba;                   /* first sector of the partition */
static uint32_t g_slots;                 /* 0: no swap */
static uint16_t *g_ref;                  /* per slot */
static uint32_t g_free_slots;
static uint32_t g_hint;
static Spinlock g_slot_lock = SPINLOCK_INIT("swap slots");

static uint64_t g_low_pages, g_high_pages;   /* the watermarks */
static uint64_t g_out, g_in;                 /* pages written / read back (the log) */

bool swap_enabled(void) { return g_slots != 0; }

void swap_init(uint32_t lba, uint32_t sectors)
{
    uint32_t n = sectors / SECTORS_PER_SLOT;
    if (n < 16) return;
    g_ref = (uint16_t *)kmalloc((uint64_t)n * sizeof(uint16_t));
    if (!g_ref) return;
    for (uint32_t i = 0; i < n; i++) g_ref[i] = 0;
    g_lba = lba;
    g_free_slots = n;
    g_slots = n;
    uint64_t total = pmm_get_total_memory() / PAGE_SIZE;
    g_low_pages = total / 32 < 4096 ? 4096 : total / 32;      /* >= 16 MB */
    g_high_pages = g_low_pages * 2;
    char b[24];
    serial_write_string("Swap: ");
    uint_to_str((uint64_t)n * PAGE_SIZE >> 20, b); serial_write_string(b);
    serial_write_string(" MB on the swap partition, reclaim below ");
    uint_to_str(g_low_pages * PAGE_SIZE >> 20, b); serial_write_string(b);
    serial_write_string(" MB free\r\n");
}

void swap_stats(uint64_t *total, uint64_t *free)
{
    *total = (uint64_t)g_slots * PAGE_SIZE;
    *free = (uint64_t)g_free_slots * PAGE_SIZE;
}

static int64_t slot_alloc(void)
{
    uint64_t f = spin_lock_irqsave(&g_slot_lock);
    int64_t got = -1;
    for (uint32_t n = 0; n < g_slots && g_free_slots; n++) {
        uint32_t i = (g_hint + n) % g_slots;
        if (!g_ref[i]) { g_ref[i] = 1; g_free_slots--; g_hint = i + 1; got = i; break; }
    }
    spin_unlock_irqrestore(&g_slot_lock, f);
    return got;
}

void swap_dup(uint32_t slot)
{
    if (slot >= g_slots) return;
    uint64_t f = spin_lock_irqsave(&g_slot_lock);
    if (g_ref[slot] && g_ref[slot] < 0xFFFF) g_ref[slot]++;
    spin_unlock_irqrestore(&g_slot_lock, f);
}

void swap_free(uint32_t slot)
{
    if (slot >= g_slots) return;
    uint64_t f = spin_lock_irqsave(&g_slot_lock);
    if (g_ref[slot] && --g_ref[slot] == 0) g_free_slots++;
    spin_unlock_irqrestore(&g_slot_lock, f);
}

bool swap_in(Process *p, const VMA *v, uint64_t page)
{
    if (!p || !p->pml4 || !v) return false;
    /* the disk first (this may sleep), then the entry as it is now: the
       big lock is held from here on, nothing changes it under us */
    kmutex_lock(&g_fs_mutex);
    PageTableEntry *e = vmm_pte_ptr(p->pml4, page);
    if (!e || !pte_swapped(*e)) {
        kmutex_unlock(&g_fs_mutex);
        return true;                          /* someone else brought it in (or dropped it) */
    }
    uint32_t slot = pte_swap_slot(*e);
    void *frame = pmm_alloc_page();
    if (!frame && swap_reclaim(BATCH)) frame = pmm_alloc_page();
    bool ok = frame && slot < g_slots &&
              ahci_read(g_lba + slot * SECTORS_PER_SLOT, SECTORS_PER_SLOT, P2V((uint64_t)(uintptr_t)frame));
    if (ok) {
        uint64_t fl = PAGE_USER;
        if (v->prot & VMA_PROT_WRITE) fl |= PAGE_WRITE;
        if (!(v->prot & VMA_PROT_EXEC)) fl |= PAGE_NX;
        vmm_map_page(p->pml4, page, (uint64_t)(uintptr_t)frame, fl);
        swap_free(slot);
        g_in++;
    } else if (frame) {
        pmm_free_page(frame);
    }
    kmutex_unlock(&g_fs_mutex);
    return ok;
}

/* ---- reclaim ---------------------------------------------------------- */

typedef struct Victim {
    PageTableEntry *e;
    uint64_t old;            /* the present entry it was */
    uint64_t swapped;        /* the swap entry it became */
    void    *frame;
    uint32_t slot;
} Victim;

static Victim g_batch[BATCH];
static uint32_t g_nbatch;
static PageTable *g_batch_pml4;

/* the batch out: entries are swap entries already, every core flushed */
static uint32_t batch_write(void)
{
    if (!g_nbatch) return 0;
    if (g_cpu_count > 1) tlb_shootdown(g_batch_pml4);
    {   /* this core too (the tables may be its own) */
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
    }
    /* Written in the big lock (the entries in the batch point into page
       tables nothing may free meanwhile), runs of consecutive slots with
       one disk command each, through a staging buffer */
    static uint8_t *stage;
    if (!stage) stage = (uint8_t *)kmalloc((uint64_t)BATCH * PAGE_SIZE);
    bool okv[BATCH];
    for (uint32_t i = 0; i < g_nbatch; ) {
        uint32_t n = 1;
        if (stage)
            while (i + n < g_nbatch && g_batch[i + n].slot == g_batch[i].slot + n) n++;
        bool ok;
        if (n > 1) {
            for (uint32_t k = 0; k < n; k++)
                memcpy(stage + (uint64_t)k * PAGE_SIZE, P2V((uint64_t)(uintptr_t)g_batch[i + k].frame), PAGE_SIZE);
            ok = ahci_write(g_lba + g_batch[i].slot * SECTORS_PER_SLOT, n * SECTORS_PER_SLOT, stage);
        } else {
            ok = ahci_write(g_lba + g_batch[i].slot * SECTORS_PER_SLOT, SECTORS_PER_SLOT,
                            P2V((uint64_t)(uintptr_t)g_batch[i].frame));
        }
        for (uint32_t k = 0; k < n; k++) okv[i + k] = ok;
        i += n;
    }
    uint32_t freed = 0;
    for (uint32_t i = 0; i < g_nbatch; i++) {
        Victim *b = &g_batch[i];
        if (okv[i]) {
            pmm_free_page(b->frame);
            freed++;
            g_out++;
        } else {
            *b->e = b->old;                   /* not written: the page stays */
            swap_free(b->slot);
        }
    }
    g_nbatch = 0;
    return freed;
}

/* one process's pages: `want` at most go out; how many did */
static uint32_t reclaim_process(Process *p, uint32_t want)
{
    PageTable *pml4 = p->pml4;
    uint32_t freed = 0;
    g_batch_pml4 = pml4;
    for (uint64_t i4 = 0; i4 < 256 && freed < want; i4++) {
        if (!(pml4->entries[i4] & PAGE_PRESENT)) continue;
        PageTable *kp = vmm_get_kernel_pml4();
        if (pml4->entries[i4] == kp->entries[i4]) continue;   /* shared with the kernel */
        PageTable *pdpt = (PageTable *)P2V(pml4->entries[i4] & PAGE_ADDR_MASK);
        for (uint64_t i3 = 0; i3 < 512 && freed < want; i3++) {
            if (!(pdpt->entries[i3] & PAGE_PRESENT) || (pdpt->entries[i3] & PAGE_PS)) continue;
            PageTable *pd = (PageTable *)P2V(pdpt->entries[i3] & PAGE_ADDR_MASK);
            for (uint64_t i2 = 0; i2 < 512 && freed < want; i2++) {
                if (!(pd->entries[i2] & PAGE_PRESENT) || (pd->entries[i2] & PAGE_PS)) continue;
                PageTable *pt = (PageTable *)P2V(pd->entries[i2] & PAGE_ADDR_MASK);
                for (uint64_t i1 = 0; i1 < 512 && freed < want; i1++) {
                    PageTableEntry *e = &pt->entries[i1];
                    uint64_t pte = *e;
                    if (!(pte & PAGE_PRESENT) || !(pte & PAGE_USER)) continue;
                    if (pte & (PAGE_COW | PAGE_SHARED_MAP)) continue;
                    if (pte & PAGE_ACCESSED) {        /* used lately: another round */
                        __atomic_and_fetch(e, ~PAGE_ACCESSED, __ATOMIC_RELAXED);
                        continue;
                    }
                    void *frame = (void *)(uintptr_t)(pte & PAGE_ADDR_MASK);
                    if (pmm_page_refcount(frame) != 1) continue;     /* someone else's too */
                    uint64_t va = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);
                    VMA *v = vma_find(p, va);
                    if (!v || (v->flags & (VMA_SHARED | VMA_GUARD))) continue;
                    int64_t slot = slot_alloc();
                    if (slot < 0) return freed;
                    uint64_t sw = swap_pte((uint32_t)slot);
                    /* the swap entry -- exchanged, so a dirty/accessed bit the
                       CPU sets meanwhile is not lost: if it changed, it stays */
                    if (!__atomic_compare_exchange_n(e, &pte, sw, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
                        swap_free((uint32_t)slot);
                        continue;
                    }
                    g_batch[g_nbatch++] = (Victim){ e, pte, sw, frame, (uint32_t)slot };
                    if (g_nbatch == BATCH) freed += batch_write();
                }
            }
        }
    }
    freed += batch_write();
    return freed;
}

uint32_t swap_reclaim(uint32_t want)
{
    if (!g_slots || !want) return 0;
    bkl_lock();                               /* the page tables (kept until the caller is done) */
    kmutex_lock(&g_fs_mutex);
    static uint32_t s_turn;                   /* whose pages first: in turn */
    uint32_t freed = 0;
    /* two rounds: the first takes the accessed marks off what it passes */
    for (int round = 0; round < 2 && freed < want; round++) {
        uint32_t n = 0;
        for (Process *p = process_list_head(); p; p = p->next) n++;
        for (uint32_t k = 0; k < n && freed < want; k++) {
            uint32_t idx = (s_turn + k) % n, j = 0;
            Process *p = process_list_head();
            while (p && j < idx) { p = p->next; j++; }
            if (!p || p->pid == 0 || !p->pml4 || p->exited || p->vfork_shared) continue;
            if (!kmutex_trylock(&p->mm_lock)) continue;   /* its pages are on the move: next */
            freed += reclaim_process(p, want - freed);
            kmutex_unlock(&p->mm_lock);
        }
        s_turn++;
    }
    kmutex_unlock(&g_fs_mutex);
    return freed;
}

static void swapd(void *arg)
{
    (void)arg;
    uint64_t last_log = 0;
    uint32_t nap = 100;
    for (;;) {
        sched_sleep_ms(nap);
        uint64_t free_pages = pmm_get_free_memory() / PAGE_SIZE;
        /* under pressure, often: a program filling memory fast eats the
           reserve between two looks */
        nap = free_pages < g_high_pages ? 20 : 100;
        if (free_pages >= g_low_pages) continue;
        while (free_pages < g_high_pages && g_free_slots) {
            uint32_t got = swap_reclaim((uint32_t)(g_high_pages - free_pages));
            if (!got) break;
            free_pages = pmm_get_free_memory() / PAGE_SIZE;
        }
        if (timer_get_ms() - last_log > 2000) {
            last_log = timer_get_ms();
            char b[24];
            serial_write_string("Swap: ");
            uint_to_str(g_out, b); serial_write_string(b);
            serial_write_string(" pages out, ");
            uint_to_str(g_in, b); serial_write_string(b);
            serial_write_string(" back in; ");
            uint_to_str((uint64_t)(g_slots - g_free_slots) * PAGE_SIZE >> 20, b); serial_write_string(b);
            serial_write_string(" MB in swap, ");
            uint_to_str(pmm_get_free_memory() >> 20, b); serial_write_string(b);
            serial_write_string(" MB free\r\n");
        }
    }
}

void swap_start_daemon(void)
{
    if (g_slots) thread_create(swapd, NULL);
}
