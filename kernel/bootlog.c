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
