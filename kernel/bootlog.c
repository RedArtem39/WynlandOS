/*
 * WynlandOS - the boot log that outlives a crash (include/wynland/bootlog.h)
 *
 * Everything the kernel sends to the serial line goes into the ring the
 * loader reserved too. Until the kernel's own page tables exist the bytes
 * wait in a small buffer here. The ring is mapped uncached: at a warm
 * reset what sat in the CPU's caches would be lost.
 */
#include <wynland/bootlog.h>
#include <wynland/boot_info.h>
#include <wynland/vmm.h>
#include <wynland/memlayout.h>

static volatile BootLog *g_log;
static char g_early[16384];
static uint32_t g_early_n;

void bootlog_putc(char c)
{
    volatile BootLog *l = g_log;
    if (l) {
        l->data[l->head % l->size] = (uint8_t)c;
        l->head++;
    } else if (g_early_n < sizeof g_early) {
        g_early[g_early_n++] = c;
    }
}

/* after vmm_init(): the loader's ring, the early bytes into it */
void bootlog_init(BootInfo *bi)
{
    if (!bi || !bi->log_addr) return;
    BootLog *l = (BootLog *)vmm_map_mmio(bi->log_addr, BOOTLOG_BYTES);
    if (!l || !bootlog_valid(l, bi->log_addr)) return;
    g_log = l;
    for (uint32_t i = 0; i < g_early_n; i++) bootlog_putc(g_early[i]);
}

void bootlog_set_state(uint32_t state)
{
    if (g_log) g_log->state = state;
}

/* the newest `len` bytes of this boot's log, for syslog(2) -- `dmesg` on a
   machine whose log can't leave it any other way */
uint32_t bootlog_read(char *dst, uint32_t len)
{
    volatile BootLog *l = g_log;
    if (!l) {
        uint32_t n = g_early_n < len ? g_early_n : len;
        for (uint32_t i = 0; i < n; i++) dst[i] = g_early[g_early_n - n + i];
        return n;
    }
    uint64_t head = l->head;
    uint64_t have = head < l->size ? head : l->size;
    uint32_t n = have < len ? (uint32_t)have : len;
    for (uint32_t i = 0; i < n; i++) dst[i] = (char)l->data[(head - n + i) % l->size];
    return n;
}

uint32_t bootlog_size(void)
{
    return g_log ? g_log->size : (uint32_t)sizeof g_early;
}

/* bytes ever logged, and the ones from `from` on (as many as the ring
   still holds) -- for kernel/sticklog.c */
uint64_t bootlog_head(void)
{
    return g_log ? g_log->head : g_early_n;
}

uint32_t bootlog_copy(uint64_t *from, char *dst, uint32_t len)
{
    volatile BootLog *l = g_log;
    uint64_t head = l ? l->head : g_early_n;
    uint64_t size = l ? l->size : sizeof g_early;
    if (head > size && *from < head - size) *from = head - size;   /* overwritten: skipped */
    uint64_t n = head - *from;
    if (n > len) n = len;
    for (uint64_t i = 0; i < n; i++)
        dst[i] = l ? (char)l->data[(*from + i) % l->size] : g_early[*from + i];
    *from += n;
    return (uint32_t)n;
}

uint32_t bootlog_boot_no(void)
{
    return g_log ? g_log->boot_no : 0;
}
