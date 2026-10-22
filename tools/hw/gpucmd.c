/*
 * WynlandOS - gpucmd: the first command an Intel Gen12 GPU runs for us,
 * from user space through /dev/wynhw (kernel/hwdev.c). Root's.
 *
 * On the blitter (BCS0, the simplest engine state) with execlists, as
 * i915 does on Gen11+: a 4 KB ring holding one MI_STORE_DATA_IMM that
 * writes 0x600DF00D through the GGTT into a page of ours. Five DMA pages
 * (status page, context: PPHWSP + register state, ring, target) mapped
 * near the top of the GGTT (the entries there put back afterwards), a
 * context image with the ring registers only (restore inhibited: no
 * engine state is loaded), submitted through the execlist submit queue.
 * Then the target page is read: the number in it means the GPU fetched
 * our ring and ran our command.
 *
 * Built on the developer's machine: gcc -O2 -static -o gpucmd gpucmd.c
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
#define WYNHW_DMA_ALLOC  0xC0105704u

typedef struct { uint8_t bus, dev, fn, size; uint16_t off, pad; uint32_t val; uint32_t pad2; } HwCfg;
typedef struct { uint8_t bus, dev, fn, bar; uint32_t flags; uint64_t phys, size; } HwBar;
typedef struct { uint64_t size, phys; } HwDma;

#define BCS        0x22000u         /* the blitter's registers */
#define MAGIC      0x600DF00Du

static int g_fd;
static volatile uint8_t *g_mmio;

static uint32_t cfg(int dev, int off)
{
    HwCfg c = { 0, (uint8_t)dev, 0, 4, (uint16_t)off, 0, 0, 0 };
    if (ioctl(g_fd, WYNHW_CFG_READ, &c) < 0) { perror("cfg"); exit(1); }
    return c.val;
}
static uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(g_mmio + r); }
static void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(g_mmio + r) = v; }
static uint32_t masked(uint32_t bits, int on) { return (bits << 16) | (on ? bits : 0); }

static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static int forcewake(uint32_t req, uint32_t ack, int on)
{
    wr(req, masked(1, on));
    uint64_t t0 = now_us();
    while (((rd(ack) & 1) != (uint32_t)on) && now_us() - t0 < 50000) ;
    return (rd(ack) & 1) == (uint32_t)on;
}

static void flush(volatile void *p, size_t n)
{
    for (size_t i = 0; i < n; i += 64) __asm__ volatile("clflush (%0)" :: "r"((volatile char *)p + i) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

static void engine_state(const char *when)
{
    printf("%s: head %08x tail %08x start %08x ctl %08x acthd %08x  execlist status %08x %08x  mode %08x mi_mode %08x\n",
           when, rd(BCS + 0x34), rd(BCS + 0x30), rd(BCS + 0x38), rd(BCS + 0x3C), rd(BCS + 0x74),
           rd(BCS + 0x234), rd(BCS + 0x238), rd(BCS + 0x29C), rd(BCS + 0x9C));
}

int main(void)
{
    g_fd = open("/dev/wynhw", O_RDWR);
    if (g_fd < 0) { perror("/dev/wynhw"); return 1; }
    int dev = -1;
    for (int d = 0; d < 32; d++)
        if ((cfg(d, 0) & 0xFFFF) == 0x8086 && (cfg(d, 8) >> 24) == 0x03) { dev = d; break; }
    if (dev < 0) { printf("no Intel display controller on bus 0\n"); return 1; }
    printf("GPU 00:%02x.0 %04x:%04x\n", dev, cfg(dev, 0) & 0xFFFF, cfg(dev, 0) >> 16);

    HwBar b0 = { 0, (uint8_t)dev, 0, 0, 0, 0, 0 };
    if (ioctl(g_fd, WYNHW_BAR, &b0) < 0) { perror("BAR0"); return 1; }
    g_mmio = mmap(NULL, b0.size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, (off_t)b0.phys);
    if (g_mmio == MAP_FAILED) { perror("mmap BAR0"); return 1; }
    volatile uint64_t *gtt = (volatile uint64_t *)(g_mmio + b0.size / 2);

    /* the GGTT's size: GGC (config 0x50) GGMS, bits 7:6 -> 2^n MB of
       entries, 8 bytes each, 4 KB of GPU addresses apiece. What the
       firmware mapped from 0 on (its framebuffer): the run of present,
       consecutive entries. Ours 1 MB below the top; the entries there
       kept to put back (unused ones hold whatever the RAM held). */
    uint32_t ggc = cfg(dev, 0x50) & 0xFFFF, ggms = (ggc >> 6) & 3;
    uint64_t entries = ggms ? ((1ull << ggms) << 20) / 8 : 0;
    uint64_t used = 0;
    while (used < entries && (gtt[used] & 1) &&
           (used == 0 || (gtt[used] & ~0xFFFull) == (gtt[used - 1] & ~0xFFFull) + 4096))
        used++;
    printf("GGTT: GGC %04x: %llu KB of entries = %llu MB of GPU addresses; the firmware maps entries 0..%llu\n",
           ggc, (unsigned long long)((entries * 8) >> 10), (unsigned long long)((entries * 4096) >> 20),
           (unsigned long long)used);
    if (entries < 4096 || entries - 256 <= used + 1024) { printf("no room in the GGTT: stopping\n"); return 1; }
    uint32_t at = (uint32_t)((entries - 256) * 4096);
    uint64_t old[5];
    for (int k = 0; k < 5; k++) old[k] = gtt[(at >> 12) + k];
    printf("ours at GGTT %#x (the entry there was %016llx)\n", at, (unsigned long long)old[0]);

    /* five pages: 0 the engine's status page, 1-2 the context (PPHWSP,
       register state), 3 the ring, 4 the target */
    HwDma dm = { 5 * 4096, 0 };
    if (ioctl(g_fd, WYNHW_DMA_ALLOC, &dm) < 0) { perror("DMA"); return 1; }
    volatile uint32_t *mem = mmap(NULL, dm.size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, (off_t)dm.phys);
    if (mem == MAP_FAILED) { perror("mmap DMA"); return 1; }
    printf("DMA: 5 pages at %#llx\n", (unsigned long long)dm.phys);
    volatile uint32_t *hwsp = mem, *regs = mem + 2048, *ring = mem + 3072, *target = mem + 4096;
    uint32_t g_hwsp = at, g_ctx = at + 0x1000, g_ring = at + 0x3000, g_target = at + 0x4000;

    if (!forcewake(0xA188, 0x130044, 1)) { printf("forcewake GT: no ack\n"); return 1; }
    if (!forcewake(0xA278, 0x0D84, 1)) { printf("forcewake render: no ack\n"); return 1; }
    engine_state("BCS before");

    /* the GPU's view: our pages in the GGTT (present | address), then the
       GGTT's own TLB flushed */
    for (int k = 0; k < 5; k++) gtt[(at >> 12) + k] = (dm.phys + (uint64_t)k * 4096) | 1;
    (void)gtt[(at >> 12) + 4];
    wr(0x101008, 1);                                  /* GFX_FLSH_CNTL_GEN6 */
    (void)rd(0x101008);

    /* the ring: store the number, then nothing more (the engine idles) */
    ring[0] = 0x10400002;                             /* MI_STORE_DATA_IMM, GGTT, one dword, 64-bit address */
    ring[1] = g_target;
    ring[2] = 0;
    ring[3] = MAGIC;
    uint32_t tail = 16;
    target[0] = 0xDEADBEEF;

    /* the register state (Gen12 xcs layout, i915's gen12_xcs_offsets) */
    uint32_t i = 0;
    regs[i++] = 0;                                    /* MI_NOOP */
    regs[i++] = 0x11000000 | (1u << 12) | (2 * 13 - 1); /* MI_LOAD_REGISTER_IMM(13), force posted */
    static const uint32_t r1[13] = { 0x244, 0x034, 0x030, 0x038, 0x03C, 0x168, 0x140, 0x110, 0x1C0, 0x1C4, 0x1C8, 0x180, 0x2B4 };
    uint32_t v1[13] = {
        masked((1u << 3) | (1u << 0), 1),             /* CONTEXT_CONTROL: inhibit syn ctx switch, engine restore inhibit */
        0, tail, g_ring, 0x1,                         /* head, tail, start, ctl: 4 KB, valid */
        0, 0, 1u << 5,                                /* BB address, BB state (PPGTT) */
        0, 0, 0, 0, 0 };
    for (int k = 0; k < 13; k++) { regs[i++] = BCS + r1[k]; regs[i++] = v1[k]; }
    for (int k = 0; k < 5; k++) regs[i++] = 0;        /* NOOP x5 */
    regs[i++] = 0x11000000 | (1u << 12) | (2 * 9 - 1); /* LRI(9) */
    static const uint32_t r2[9] = { 0x3A8, 0x28C, 0x288, 0x284, 0x280, 0x27C, 0x278, 0x274, 0x270 };
    for (int k = 0; k < 9; k++) { regs[i++] = BCS + r2[k]; regs[i++] = 0; }
    regs[i++] = 0x05000000 | 1;                       /* MI_BATCH_BUFFER_END (gen11+: bit 0) */
    flush(mem, dm.size);

    /* execlists on the blitter: run-list mode, legacy off, ring not
       stopped, its status page */
    wr(BCS + 0x98, 0xFFFFFFFF);                       /* HWSTAM: no interrupts */
    wr(BCS + 0x29C, masked((1u << 15) | (1u << 3), 1));
    wr(BCS + 0x9C, masked(1u << 8, 0));
    wr(BCS + 0x80, g_hwsp);
    (void)rd(BCS + 0x80);
    engine_state("BCS ready");

    /* the context descriptor: valid, legacy 32-bit addressing, privileged,
       its image in the GGTT; software id 1 */
    uint64_t desc = (uint64_t)g_ctx | 1u | (1u << 3) | (1u << 8) | (1ull << 37);
    wr(BCS + 0x510, (uint32_t)desc);
    wr(BCS + 0x514, (uint32_t)(desc >> 32));
    wr(BCS + 0x550, 1);                               /* EL_CTRL_LOAD */
    printf("submitted: descriptor %016llx\n", (unsigned long long)desc);

    uint64_t t0 = now_us();
    uint32_t got = 0;
    while (now_us() - t0 < 1000000) {
        flush(target, 64);
        got = target[0];
        if (got == MAGIC) break;
        usleep(1000);
    }
    engine_state("BCS after");
    if (got == MAGIC)
        printf("THE GPU RAN OUR COMMAND: target = %08x after %llu us\n", got, (unsigned long long)(now_us() - t0));
    else {
        printf("no: target = %08x after 1 s\n", got);
        printf("errors: eir %08x esr %08x ipeir %08x ipehr %08x  fault %08x  csb ptr %08x\n",
               rd(BCS + 0xB0), rd(BCS + 0xB8), rd(BCS + 0x64), rd(BCS + 0x68), rd(0xCEC4), rd(BCS + 0x3A0));
    }
    flush(hwsp, 4096);
    printf("status page: %08x %08x %08x %08x ... at 0x40: %08x %08x %08x %08x\n",
           hwsp[0], hwsp[1], hwsp[2], hwsp[3], hwsp[16], hwsp[17], hwsp[18], hwsp[19]);

    /* the GGTT entries put back (the engine is idle, or hung: the next
       step is resetting it), then the GPU let sleep */
    for (int k = 0; k < 5; k++) gtt[(at >> 12) + k] = old[k];
    wr(0x101008, 1);
    forcewake(0xA278, 0x0D84, 0);
    forcewake(0xA188, 0x130044, 0);
    printf("gpucmd: done\n");
    return got == MAGIC ? 0 : 2;
}
