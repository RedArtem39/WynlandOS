/*
 * WynlandOS - gpuprobe: the first look at an Intel GPU (Gen12, Tiger Lake)
 * from user space, through /dev/wynhw (kernel/hwdev.c). Root's.
 *
 * Reads only, but for forcewake (the render well held awake for a moment
 * and released): the device, its BARs, the display's current plane and,
 * through the GGTT, where in RAM the picture it scans out lives; the render
 * engine's ring registers and its timestamp ticking.
 *
 * Built on the developer's machine: gcc -O2 -static -o gpuprobe gpuprobe.c
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define WYNHW_CFG_READ   0xC0105701u
#define WYNHW_BAR        0xC0185703u

typedef struct { uint8_t bus, dev, fn, size; uint16_t off, pad; uint32_t val; uint32_t pad2; } HwCfg;
typedef struct { uint8_t bus, dev, fn, bar; uint32_t flags; uint64_t phys, size; } HwBar;

static int g_fd;
static volatile uint8_t *g_mmio;

static uint32_t cfg(int dev, int fn, int off)
{
    HwCfg c = { 0, (uint8_t)dev, (uint8_t)fn, 4, (uint16_t)off, 0, 0, 0 };
    if (ioctl(g_fd, WYNHW_CFG_READ, &c) < 0) { perror("cfg read"); exit(1); }
    return c.val;
}

static uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(g_mmio + r); }
static void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(g_mmio + r) = v; }

static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* a masked register (forcewake): bits 31:16 say which of 15:0 to write */
static int forcewake(uint32_t req, uint32_t ack, int on)
{
    wr(req, (1u << 16) | (on ? 1u : 0u));
    uint64_t t0 = now_us();
    while (((rd(ack) & 1) != (uint32_t)on) && now_us() - t0 < 50000) ;
    return (rd(ack) & 1) == (uint32_t)on;
}

int main(void)
{
    g_fd = open("/dev/wynhw", O_RDWR);
    if (g_fd < 0) { perror("/dev/wynhw (root's: run with ary)"); return 1; }

    /* the display controller on bus 0 (the iGPU is 00:02.0) */
    int dev = -1;
    for (int d = 0; d < 32; d++) {
        uint32_t id = cfg(d, 0, 0);
        if ((id & 0xFFFF) == 0x8086 && (cfg(d, 0, 8) >> 24) == 0x03) { dev = d; break; }
    }
    if (dev < 0) {
        printf("no Intel display controller on bus 0\n");
        /* a VM's std VGA (1234:1111): its BAR2 holds the Bochs VBE
           registers, ID at 0x500 -- /dev/wynhw's BAR mapping checked */
        for (int d = 0; d < 32; d++) {
            if (cfg(d, 0, 0) != 0x11111234) continue;
            HwBar b = { 0, (uint8_t)d, 0, 2, 0, 0, 0 };
            if (ioctl(g_fd, WYNHW_BAR, &b) < 0) { perror("std VGA BAR2"); return 1; }
            volatile uint8_t *m = mmap(NULL, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, (off_t)b.phys);
            if (m == MAP_FAILED) { perror("mmap std VGA BAR2"); return 1; }
            uint16_t vid = *(volatile uint16_t *)(m + 0x500), xres = *(volatile uint16_t *)(m + 0x502);
            printf("std VGA 00:%02x.0: BAR2 %#llx %llu KB, Bochs VBE id %#x (%s), x resolution %u\n", d,
                   (unsigned long long)b.phys, (unsigned long long)(b.size >> 10), vid,
                   (vid & 0xFFF0) == 0xB0C0 ? "BAR mapping works" : "UNEXPECTED", xres);
        }
        return 1;
    }
    uint32_t id = cfg(dev, 0, 0), cr = cfg(dev, 0, 8);
    printf("GPU 00:%02x.0  %04x:%04x  revision %02x  class %06x  command %04x\n",
           dev, id & 0xFFFF, id >> 16, cr & 0xFF, cr >> 8, cfg(dev, 0, 4) & 0xFFFF);

    HwBar b0 = { 0, (uint8_t)dev, 0, 0, 0, 0, 0 }, b2 = { 0, (uint8_t)dev, 0, 2, 0, 0, 0 };
    if (ioctl(g_fd, WYNHW_BAR, &b0) < 0) { perror("BAR0"); return 1; }
    printf("BAR0 GTTMMADR  %#llx  %llu MB  flags %x\n", (unsigned long long)b0.phys, (unsigned long long)(b0.size >> 20), b0.flags);
    if (ioctl(g_fd, WYNHW_BAR, &b2) == 0)
        printf("BAR2 GMADR     %#llx  %llu MB  flags %x (the aperture)\n", (unsigned long long)b2.phys, (unsigned long long)(b2.size >> 20), b2.flags);

    g_mmio = mmap(NULL, b0.size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, (off_t)b0.phys);
    if (g_mmio == MAP_FAILED) { perror("mmap BAR0"); return 1; }
    volatile uint64_t *gtt = (volatile uint64_t *)(g_mmio + b0.size / 2);   /* gen8+: the upper half */

    /* the display: per pipe, its source size and plane 1 */
    static const char pn[] = "ABCD";
    for (int p = 0; p < 4; p++) {
        uint32_t conf = rd(0x70008 + p * 0x1000);
        uint32_t src = rd(0x6001C + p * 0x1000);
        uint32_t pctl = rd(0x70180 + p * 0x1000), stride = rd(0x70188 + p * 0x1000), surf = rd(0x7019C + p * 0x1000);
        printf("pipe %c: conf %08x %s  source %ux%u  plane1 ctl %08x %s  stride %u x 64 B  surface (GGTT) %#x\n",
               pn[p], conf, conf >> 31 ? "on" : "off", ((src >> 16) & 0xFFFF) + 1, (src & 0xFFFF) + 1,
               pctl, pctl >> 31 ? "on" : "off", stride & 0x7FF, surf & ~0xFFFu);
        if ((pctl >> 31) && (conf >> 31)) {
            uint64_t pte = gtt[(surf & ~0xFFFu) >> 12];
            printf("        its GGTT entry %016llx: %s, RAM at %#llx  (the firmware's framebuffer)\n",
                   (unsigned long long)pte, pte & 1 ? "present" : "NOT present",
                   (unsigned long long)(pte & 0x7FFFFFF000ull));
        }
    }
    printf("GGTT entry 0: %016llx\n", (unsigned long long)gtt[0]);


    /* forcewake the render well: its registers read real only while awake */
    printf("forcewake acks before: render %08x  gt %08x\n", rd(0x0D84), rd(0x130044));
    if (!forcewake(0xA278, 0x0D84, 1)) {
        printf("forcewake render: NO ACK in 50 ms\n");
    } else {
        printf("forcewake render: awake\n");
        /* fuses (they read as 0 while the GT sleeps): what this GT has */
        printf("fuses: mirror_fuse3 %08x  subslice-disable %08x  eu-disable %08x\n", rd(0x9118), rd(0x913C), rd(0x9134));
        printf("RCS ring: head %08x tail %08x start %08x ctl %08x  acthd %08x\n",
               rd(0x2034), rd(0x2030), rd(0x2038), rd(0x203C), rd(0x2074));
        printf("RCS mode: mi_mode %08x  ring_mode %08x  execlist status %08x %08x  hws_pga %08x\n",
               rd(0x209C), rd(0x229C), rd(0x2234), rd(0x2238), rd(0x2080));
        uint32_t t1 = rd(0x2358);
        usleep(10000);
        uint32_t t2 = rd(0x2358);
        printf("RCS timestamp: %u -> %u in 10 ms (%s)\n", t1, t2, t2 != t1 ? "the engine's clock runs" : "STANDS STILL");
        printf("forcewake render released: %s\n", forcewake(0xA278, 0x0D84, 0) ? "yes" : "NO ACK");
    }
    printf("gpuprobe: done\n");
    return 0;
}
