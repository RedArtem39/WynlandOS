/*
 * WynlandOS - /dev/wynhw: a device driver's raw access, from user space.
 *
 * For bringing up hardware (the GPU first) with a plain program that can be
 * rebuilt and rerun without a reboot -- what UIO/VFIO are on Linux, minus
 * the IOMMU. Root's only (CAP_SYS_RAWIO, outside any sandbox):
 *   ioctl WYNHW_CFG_READ / WYNHW_CFG_WRITE   PCI configuration space
 *   ioctl WYNHW_BAR                          a BAR's address and size
 *   ioctl WYNHW_DMA_ALLOC / WYNHW_DMA_FREE   physically contiguous memory
 *                                            and its physical address
 *   mmap(offset = a physical address)        a BAR asked for before
 *                                            (uncached; a prefetchable one
 *                                            write-combining), or a DMA
 *                                            buffer (write-back): nothing else
 * A device driven this way can still write anywhere in memory by DMA: the
 * program is trusted as much as the kernel is.
 */
#include <wynland/types.h>
#include <wynland/pci.h>
#include <wynland/pmm.h>
#include <wynland/vmm.h>
#include <wynland/memlayout.h>
#include <wynland/spinlock.h>

#define WYNHW_CFG_READ   0xC0105701u
#define WYNHW_CFG_WRITE  0xC0105702u
#define WYNHW_BAR        0xC0185703u
#define WYNHW_DMA_ALLOC  0xC0105704u
#define WYNHW_DMA_FREE   0xC0105705u

typedef struct { uint8_t bus, dev, fn, size; uint16_t off, pad; uint32_t val; uint32_t pad2; } HwCfg;      /* 16 */
typedef struct { uint8_t bus, dev, fn, bar; uint32_t flags; uint64_t phys, size; } HwBar;                 /* 24 */
typedef struct { uint64_t size, phys; } HwDma;                                                            /* 16 */

#define HW_MAX_RANGES 64
static struct { uint64_t phys, size; uint8_t kind; } g_hw[HW_MAX_RANGES];   /* kind: 1 BAR uc, 2 BAR wc, 3 DMA */
static Spinlock g_hw_lock;

#include <wynland/usercopy.h>

static void hw_note(uint64_t phys, uint64_t size, uint8_t kind)
{
    uint64_t fl = spin_lock_irqsave(&g_hw_lock);
    for (int i = 0; i < HW_MAX_RANGES; i++)
        if (g_hw[i].kind == kind && g_hw[i].phys == phys) { spin_unlock_irqrestore(&g_hw_lock, fl); return; }
    for (int i = 0; i < HW_MAX_RANGES; i++)
        if (!g_hw[i].kind) { g_hw[i].phys = phys; g_hw[i].size = size; g_hw[i].kind = kind; break; }
    spin_unlock_irqrestore(&g_hw_lock, fl);
}

/* a BAR's address and size: all ones written (memory decode off meanwhile,
   as at enumeration), the size read back, the BAR restored */
static int hw_bar(HwBar *b)
{
    if (b->bar > 5 || b->dev > 31 || b->fn > 7) return -22;
    uint8_t off = (uint8_t)(0x10 + b->bar * 4);
    uint32_t lo = pci_read_config(b->bus, b->dev, b->fn, off);
    if (lo & 1) return -22;                         /* I/O ports: not this */
    bool is64 = ((lo >> 1) & 3) == 2;
    uint32_t hi = is64 ? pci_read_config(b->bus, b->dev, b->fn, off + 4) : 0;
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    uint32_t cmd = pci_read_config(b->bus, b->dev, b->fn, 0x04);
    pci_write_config(b->bus, b->dev, b->fn, 0x04, cmd & ~2u);
    pci_write_config(b->bus, b->dev, b->fn, off, 0xFFFFFFFFu);
    uint32_t slo = pci_read_config(b->bus, b->dev, b->fn, off);
    uint32_t shi = 0xFFFFFFFFu;
    if (is64) {
        pci_write_config(b->bus, b->dev, b->fn, off + 4, 0xFFFFFFFFu);
        shi = pci_read_config(b->bus, b->dev, b->fn, off + 4);
        pci_write_config(b->bus, b->dev, b->fn, off + 4, hi);
    }
    pci_write_config(b->bus, b->dev, b->fn, off, lo);
    pci_write_config(b->bus, b->dev, b->fn, 0x04, cmd);
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
    uint64_t mask = ((uint64_t)shi << 32) | (slo & ~0xFull);
    if (!is64) mask |= 0xFFFFFFFF00000000ull;
    b->phys = ((uint64_t)hi << 32) | (lo & ~0xFull);
    b->size = ~mask + 1;
    b->flags = lo & 0xF;
    if (!b->phys || !b->size) return -19;           /* -ENODEV: not there */
    hw_note(b->phys, b->size, (lo & 8) ? 2 : 1);
    return 0;
}

int hwdev_ioctl(uint64_t req, void *argp)
{
    if (req == WYNHW_CFG_READ || req == WYNHW_CFG_WRITE) {
        HwCfg c;
        if (copy_from_user(&c, argp, sizeof c)) return -14;
        if (c.dev > 31 || c.fn > 7 || c.off > 252 || (c.off & 3)) return -22;
        if (req == WYNHW_CFG_WRITE) pci_write_config(c.bus, c.dev, c.fn, (uint8_t)c.off, c.val);
        c.val = pci_read_config(c.bus, c.dev, c.fn, (uint8_t)c.off);
        return copy_to_user(argp, &c, sizeof c) ? -14 : 0;
    }
    if (req == WYNHW_BAR) {
        HwBar b;
        if (copy_from_user(&b, argp, sizeof b)) return -14;
        int r = hw_bar(&b);
        if (r) return r;
        return copy_to_user(argp, &b, sizeof b) ? -14 : 0;
    }
    if (req == WYNHW_DMA_ALLOC) {
        HwDma d;
        if (copy_from_user(&d, argp, sizeof d)) return -14;
        if (!d.size || d.size > (64ull << 20)) return -22;
        uint32_t pages = (uint32_t)((d.size + PAGE_SIZE - 1) / PAGE_SIZE);
        void *p = pmm_alloc_contiguous(pages);
        if (!p) return -12;
        uint64_t phys = (uint64_t)(uintptr_t)p;
        uint8_t *v = (uint8_t *)P2V(phys);
        for (uint64_t i = 0; i < (uint64_t)pages * PAGE_SIZE; i++) v[i] = 0;
        d.phys = phys;
        d.size = (uint64_t)pages * PAGE_SIZE;
        hw_note(phys, d.size, 3);
        return copy_to_user(argp, &d, sizeof d) ? -14 : 0;
    }
    if (req == WYNHW_DMA_FREE) {
        HwDma d;
        if (copy_from_user(&d, argp, sizeof d)) return -14;
        uint64_t fl = spin_lock_irqsave(&g_hw_lock);
        for (int i = 0; i < HW_MAX_RANGES; i++) {
            if (g_hw[i].kind == 3 && g_hw[i].phys == d.phys) {
                uint64_t size = g_hw[i].size;
                g_hw[i].kind = 0;
                spin_unlock_irqrestore(&g_hw_lock, fl);
                /* the pages back -- the caller has made sure the device is
                   done with them, and unmapped them */
                for (uint64_t o = 0; o < size; o += PAGE_SIZE) pmm_free_page((void *)(uintptr_t)(d.phys + o));
                return 0;
            }
        }
        spin_unlock_irqrestore(&g_hw_lock, fl);
        return -22;
    }
    return -25;                                     /* -ENOTTY */
}

/* mmap: the range must lie inside one noted BAR or DMA buffer; its page
   cache flags back */
int hwdev_mmap_check(uint64_t phys, uint64_t len, uint64_t *cache)
{
    if (phys & (PAGE_SIZE - 1)) return -22;
    uint64_t fl = spin_lock_irqsave(&g_hw_lock);
    for (int i = 0; i < HW_MAX_RANGES; i++) {
        if (!g_hw[i].kind || phys < g_hw[i].phys || phys + len > g_hw[i].phys + g_hw[i].size) continue;
        uint8_t k = g_hw[i].kind;
        spin_unlock_irqrestore(&g_hw_lock, fl);
        *cache = k == 1 ? (PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH) : k == 2 ? PAGE_PAT : 0;
        return 0;
    }
    spin_unlock_irqrestore(&g_hw_lock, fl);
    return -22;
}
