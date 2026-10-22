/*
 * WynlandOS - what the GPU bring-up programs share (tools/hw/gpu*.c):
 * an Intel Gen12 GPU from user space through /dev/wynhw (kernel/hwdev.c).
 *
 *   gpu_open()            the device, BAR0 mapped, the GGTT found
 *   gpu_ggtt_map()        DMA pages into the GGTT near its top, guard
 *                         entries after them (a scratch page), the old
 *                         entries kept; gpu_ggtt_unmap() puts them back
 *   gpu_lrc()             a context image: Gen12 ring context (i915's
 *                         gen12_*_offsets, the first two LRI blocks),
 *                         restore inhibited -- no engine state is loaded
 *   gpu_submit()          execlists on an engine, one context, waited for
 *
 * Built into each program (static functions): gcc -O2 -static.
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

#define RCS 0x2000u
#define BCS 0x22000u

static int g_fd, g_dev = -1;
static volatile uint8_t *g_mmio;
static volatile uint64_t *g_gtt;
static uint64_t g_gtt_entries, g_gtt_fw;

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

static void flush(volatile void *p, size_t n)
{
    for (size_t i = 0; i < n; i += 64) __asm__ volatile("clflush (%0)" :: "r"((volatile char *)p + i) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

static int forcewake(uint32_t req, uint32_t ack, int on)
{
    wr(req, masked(1, on));
    uint64_t t0 = now_us();
    while (((rd(ack) & 1) != (uint32_t)on) && now_us() - t0 < 50000) ;
    return (rd(ack) & 1) == (uint32_t)on;
}
static int gpu_wake(int on)
{
    if (on) return forcewake(0xA188, 0x130044, 1) && forcewake(0xA278, 0x0D84, 1);
    forcewake(0xA278, 0x0D84, 0);
    forcewake(0xA188, 0x130044, 0);
    return 1;
}

static void *dma(uint64_t bytes, uint64_t *phys)
{
    HwDma d = { bytes, 0 };
    if (ioctl(g_fd, WYNHW_DMA_ALLOC, &d) < 0) { perror("DMA"); exit(1); }
    void *v = mmap(NULL, d.size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, (off_t)d.phys);
    if (v == MAP_FAILED) { perror("mmap DMA"); exit(1); }
    *phys = d.phys;
    return v;
}

static void gpu_open(void)
{
    g_fd = open("/dev/wynhw", O_RDWR);
    if (g_fd < 0) { perror("/dev/wynhw"); exit(1); }
    for (int d = 0; d < 32; d++)
        if ((cfg(d, 0) & 0xFFFF) == 0x8086 && (cfg(d, 8) >> 24) == 0x03) { g_dev = d; break; }
    if (g_dev < 0) { printf("no Intel display controller on bus 0\n"); exit(1); }
    HwBar b0 = { 0, (uint8_t)g_dev, 0, 0, 0, 0, 0 };
    if (ioctl(g_fd, WYNHW_BAR, &b0) < 0) { perror("BAR0"); exit(1); }
    g_mmio = mmap(NULL, b0.size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, (off_t)b0.phys);
    if (g_mmio == MAP_FAILED) { perror("mmap BAR0"); exit(1); }
    g_gtt = (volatile uint64_t *)(g_mmio + b0.size / 2);
    uint32_t ggms = (cfg(g_dev, 0x50) >> 6) & 3;
    g_gtt_entries = ggms ? ((1ull << ggms) << 20) / 8 : 0;
    while (g_gtt_fw < g_gtt_entries && (g_gtt[g_gtt_fw] & 1) &&
           (g_gtt_fw == 0 || (g_gtt[g_gtt_fw] & ~0xFFFull) == (g_gtt[g_gtt_fw - 1] & ~0xFFFull) + 4096))
        g_gtt_fw++;
    printf("GPU 00:%02x.0 %04x:%04x; GGTT %llu MB of addresses, the firmware's 0..%llu\n", g_dev,
           cfg(g_dev, 0) & 0xFFFF, cfg(g_dev, 0) >> 16, (unsigned long long)((g_gtt_entries * 4096) >> 20),
           (unsigned long long)g_gtt_fw);
}

/* GGTT: `pages` pages from `phys` at 4 MB below the top, then 64 guard
   entries on a scratch page; the old entries kept */
#define GUARD 64
static uint64_t g_old[1024 + GUARD];
static uint32_t g_map_at, g_map_n;

static uint32_t gpu_ggtt_map(uint64_t phys, uint32_t pages)
{
    if (pages + GUARD > 1024 || g_gtt_entries < 8192 || g_gtt_entries - 1024 <= g_gtt_fw + 1024) {
        printf("no room in the GGTT\n");
        exit(1);
    }
    static uint64_t scratch;
    if (!scratch) dma(4096, &scratch);
    g_map_at = (uint32_t)(g_gtt_entries - 1024);
    g_map_n = pages + GUARD;
    for (uint32_t k = 0; k < g_map_n; k++) g_old[k] = g_gtt[g_map_at + k];
    for (uint32_t k = 0; k < g_map_n; k++)
        g_gtt[g_map_at + k] = (k < pages ? phys + (uint64_t)k * 4096 : scratch) | 1;
    (void)g_gtt[g_map_at + g_map_n - 1];
    wr(0x101008, 1);                                  /* GFX_FLSH_CNTL_GEN6: the GGTT's TLB */
    (void)rd(0x101008);
    return g_map_at * 4096u;
}

static void gpu_ggtt_unmap(void)
{
    for (uint32_t k = 0; k < g_map_n; k++) g_gtt[g_map_at + k] = g_old[k];
    wr(0x101008, 1);
    (void)rd(0x101008);
}

/* the context image at `regs` (the page after the PPHWSP): ring context
   for `base`'s engine; pml4: 4-level PPGTT root (0: none) */
static void gpu_lrc(volatile uint32_t *regs, uint32_t base, uint32_t ring_ggtt, uint32_t tail, uint64_t pml4)
{
    uint32_t i = 0;
    regs[i++] = 0;                                    /* MI_NOOP */
    regs[i++] = 0x11000000 | (1u << 12) | (2 * 13 - 1); /* MI_LOAD_REGISTER_IMM(13), force posted */
    static const uint32_t r1[13] = { 0x244, 0x034, 0x030, 0x038, 0x03C, 0x168, 0x140, 0x110, 0x1C0, 0x1C4, 0x1C8, 0x180, 0x2B4 };
    uint32_t v1[13] = {
        masked((1u << 3) | (1u << 0), 1),             /* CONTEXT_CONTROL: inhibit syn ctx switch, engine restore inhibit */
        0, tail, ring_ggtt, 0x1,                      /* head, tail, start, ctl: 4 KB, valid */
        0, 0, 1u << 5,                                /* BB address, BB state: PPGTT */
        0, 0, 0, 0, 0 };
    for (int k = 0; k < 13; k++) { regs[i++] = base + r1[k]; regs[i++] = v1[k]; }
    for (int k = 0; k < 5; k++) regs[i++] = 0;        /* NOOP x5 */
    regs[i++] = 0x11000000 | (1u << 12) | (2 * 9 - 1); /* LRI(9) */
    static const uint32_t r2[9] = { 0x3A8, 0x28C, 0x288, 0x284, 0x280, 0x27C, 0x278, 0x274, 0x270 };
    uint32_t v2[9] = { 0, 0, 0, 0, 0, 0, 0, (uint32_t)(pml4 >> 32), (uint32_t)pml4 };   /* PDP0 = the PML4 */
    for (int k = 0; k < 9; k++) { regs[i++] = base + r2[k]; regs[i++] = v2[k]; }
    regs[i++] = 0x05000000 | 1;                       /* MI_BATCH_BUFFER_END (gen11+: bit 0) */
}

static void engine_state(uint32_t base, const char *when)
{
    printf("%s: head %08x tail %08x start %08x acthd %08x  execlists %08x %08x  mode %08x mi_mode %08x\n",
           when, rd(base + 0x34), rd(base + 0x30), rd(base + 0x38), rd(base + 0x74),
           rd(base + 0x234), rd(base + 0x238), rd(base + 0x29C), rd(base + 0x9C));
}

/* execlists on the engine, its status page; one context submitted:
   legacy 32-bit or (pml4) 4-level PPGTT addressing; then waited for
   *done == want (clflushed), up to 1 s */
static int gpu_submit(uint32_t base, uint32_t hwsp_ggtt, uint32_t ctx_ggtt, int ppgtt48,
                      volatile uint32_t *done, uint32_t want)
{
    wr(base + 0x98, 0xFFFFFFFF);                      /* HWSTAM: no interrupts */
    wr(base + 0x29C, masked((1u << 15) | (1u << 3), 1));   /* run lists on, legacy mode off */
    wr(base + 0x9C, masked(1u << 8, 0));              /* MI_MODE: not stopped */
    wr(base + 0x80, hwsp_ggtt);
    (void)rd(base + 0x80);
    engine_state(base, "ready");
    uint64_t desc = (uint64_t)ctx_ggtt | 1u | ((ppgtt48 ? 3u : 1u) << 3) | (1u << 8) | (1ull << 37);
    wr(base + 0x510, (uint32_t)desc);
    wr(base + 0x514, (uint32_t)(desc >> 32));
    wr(base + 0x550, 1);                              /* EL_CTRL_LOAD */
    printf("submitted: descriptor %016llx\n", (unsigned long long)desc);
    uint64_t t0 = now_us();
    uint32_t got = 0;
    while (now_us() - t0 < 1000000) {
        flush(done, 64);
        got = *done;
        if (got == want) break;
        usleep(1000);
    }
    engine_state(base, "after");
    if (got == want) {
        printf("DONE: %08x after %llu us\n", got, (unsigned long long)(now_us() - t0));
        return 1;
    }
    printf("NOT DONE: %08x after 1 s\n", got);
    printf("errors: eir %08x esr %08x ipeir %08x ipehr %08x  fault %08x  csb ptr %08x  instdone %08x\n",
           rd(base + 0xB0), rd(base + 0xB8), rd(base + 0x64), rd(base + 0x68), rd(0xCEC4), rd(base + 0x3A0),
           rd(base + 0x6C));
    return 0;
}
