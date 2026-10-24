/*
 * WynlandOS - Intel Gen12 graphics (Tiger Lake), the kernel's side.
 *
 * Render only: the display stays the firmware's (simpledrm scans its
 * framebuffer out). Every piece here was first proven on the real machine
 * from user space through /dev/wynhw (tools/hw/gpu*.c, test-wyn 1-5):
 *
 *   forcewake         the render and GT wells held awake while used
 *   GGTT              the firmware's mapping (its framebuffer) left as it
 *                     is; ours handed out above it, page by page
 *   PAT               cache types by index as i915 has them (0 WB, 3 UC)
 *   engines           RCS (render) and BCS (blitter): execlists, a status
 *                     page each; a request ends in a seqno stored there
 *   contexts          an image (PPHWSP + register state: Gen12's ring
 *                     context layout) and a ring; the first submission
 *                     with restore inhibited, the later ones restoring what
 *                     the engine saved; the PPGTT root set from the ring
 *                     (a restore-inhibited context does not load it)
 *
 * Off unless boot.cfg says gpu=1: then a self-test at boot -- two requests
 * on one context per engine, the second restoring the first's state.
 * Next: GEM objects and the i915 uAPI for Mesa (iris).
 */
#include <wynland/types.h>
#include <wynland/pci.h>
#include <wynland/pmm.h>
#include <wynland/vmm.h>
#include <wynland/memlayout.h>
#include <wynland/spinlock.h>
#include <wynland/sched.h>
#include <wynland/i915.h>
#include <wynland/waitqueue.h>
#include <wynland/smp.h>

extern void serial_write_string(const char *s);
extern void uint_to_hex(uint64_t v, char *buf);
extern uint64_t timer_get_ms(void);
extern void *memset(void *d, int c, size_t n);

#define RCS I915_RCS
#define BCS I915_BCS

static volatile uint8_t  *g_mmio;
static volatile uint64_t *g_gtt;
static uint64_t g_gtt_entries, g_gtt_fw, g_gtt_next;
static Spinlock g_lock;
static int g_fw_refs;
static bool g_up;
static int g_dev = -1;                                /* its PCI slot (bus 0) */
static volatile bool g_irq_on;                        /* MSI in, the GT's user interrupts on */
static volatile uint64_t g_irqs;                      /* interrupts taken (the log says if they come) */
static WaitQueue g_eng_wq[2];                         /* render, copy: woken when a request ends */
static bool g_clock;                                  /* gt_clock_up() at all (boot.cfg gpuclock=0: no) */
static uint32_t g_devid, g_rev, g_subsys;

static void say(const char *s, uint64_t v)
{
    char b[20];
    serial_write_string("i915: ");
    serial_write_string(s);
    uint_to_hex(v, b);
    serial_write_string(b);
    serial_write_string("\r\n");
}

static uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(g_mmio + r); }
static void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(g_mmio + r) = v; }
static void mocs_init(void);
static void rcs_workarounds(void);
static void gt_clock_up(void);
static void irq_gt_enable(void);
static uint32_t masked(uint32_t bits, int on) { return (bits << 16) | (on ? bits : 0); }

static void flush(volatile void *p, uint64_t n)
{
    for (uint64_t i = 0; i < n; i += 64) __asm__ volatile("clflush (%0)" :: "r"((volatile char *)p + i) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

/* ---- forcewake: render + GT, counted ---------------------------------- */

static bool fw_wait(uint32_t ack, uint32_t want)
{
    uint64_t t0 = timer_get_ms();
    while ((rd(ack) & 1) != want)
        if (timer_get_ms() - t0 > 50) return false;
    return true;
}

static bool fw_get(void)
{
    uint64_t fl = spin_lock_irqsave(&g_lock);
    bool ok = true;
    if (g_fw_refs++ == 0) {
        wr(0xA188, masked(1, 1));
        wr(0xA278, masked(1, 1));
        ok = fw_wait(0x130044, 1) && fw_wait(0x0D84, 1);
        if (!ok) {                                    /* (callers return without fw_put then) */
            wr(0xA278, masked(1, 0));
            wr(0xA188, masked(1, 0));
            g_fw_refs--;
        }
    }
    spin_unlock_irqrestore(&g_lock, fl);
    return ok;
}

static void fw_put(void)
{
    uint64_t fl = spin_lock_irqsave(&g_lock);
    if (--g_fw_refs == 0) {
        wr(0xA278, masked(1, 0));
        wr(0xA188, masked(1, 0));
    }
    spin_unlock_irqrestore(&g_lock, fl);
}

/* ---- memory: contiguous pages, and their place in the GGTT ------------ */


static bool gpu_mem(GpuMem *m, uint32_t pages)
{
    void *p = pmm_alloc_contiguous(pages);
    if (!p) return false;
    m->phys = (uint64_t)(uintptr_t)p;
    m->v = (volatile uint32_t *)P2V(m->phys);
    m->pages = pages;
    memset((void *)m->v, 0, (size_t)pages * 4096);
    uint64_t fl = spin_lock_irqsave(&g_lock);
    if (g_gtt_next + pages > g_gtt_entries) {
        spin_unlock_irqrestore(&g_lock, fl);
        say("out of GGTT, entries: ", g_gtt_entries);
        return false;
    }
    uint64_t e = g_gtt_next;
    g_gtt_next += pages;
    spin_unlock_irqrestore(&g_lock, fl);
    for (uint32_t k = 0; k < pages; k++) g_gtt[e + k] = (m->phys + (uint64_t)k * 4096) | 1;
    (void)g_gtt[e + pages - 1];
    wr(0x101008, 1);                                  /* GFX_FLSH_CNTL_GEN6: the GGTT's TLB */
    (void)rd(0x101008);
    m->ggtt = (uint32_t)(e * 4096);
    return true;
}

/* pages already allocated (an object's) at a GGTT place aligned to
   `align` pages -- for the display, which reads through the GGTT. The
   place is never given back (a bump allocator): callers keep it. 0: no room. */
uint32_t i915_ggtt_map(uint64_t phys, uint32_t pages, uint32_t align)
{
    uint64_t fl = spin_lock_irqsave(&g_lock);
    uint64_t e = (g_gtt_next + align - 1) & ~(uint64_t)(align - 1);
    if (e + pages > g_gtt_entries || e * 4096 > 0xFFFFFFFFull) { spin_unlock_irqrestore(&g_lock, fl); return 0; }
    g_gtt_next = e + pages;
    spin_unlock_irqrestore(&g_lock, fl);
    for (uint32_t k = 0; k < pages; k++) g_gtt[e + k] = (phys + (uint64_t)k * 4096) | 1;
    (void)g_gtt[e + pages - 1];
    wr(0x101008, 1);                                  /* GFX_FLSH_CNTL_GEN6: the GGTT's TLB */
    (void)rd(0x101008);
    return (uint32_t)(e * 4096);
}

/* ---- engines and contexts --------------------------------------------- */


Engine i915_rcs = { RCS, "RCS", { 0 }, 0, false };
Engine i915_bcs = { BCS, "BCS", { 0 }, 0, false };
#define g_rcs i915_rcs
#define g_bcs i915_bcs


static bool engine_init(Engine *e)
{
    if (!e->hwsp.phys && !gpu_mem(&e->hwsp, 1)) return false;
    wr(e->base + 0x98, 0xFFFFFFFF);                   /* HWSTAM: no interrupts (polled) */
    wr(e->base + 0x29C, masked((1u << 15) | (1u << 3), 1));   /* run lists, no legacy mode */
    wr(e->base + 0x9C, masked(1u << 8, 0));           /* MI_MODE: not stopped */
    wr(e->base + 0x80, e->hwsp.ggtt);
    (void)rd(e->base + 0x80);
    e->up = true;
    return true;
}

/* Gen12's ring context (i915's gen12_*_offsets, the first two LRI blocks) */
static void ctx_regs(GpuCtx *c, bool inhibit)
{
    volatile uint32_t *r = c->img.v + 1024;
    uint32_t base = c->e->base, i = 0;
    r[i++] = 0;
    r[i++] = 0x11000000 | (1u << 12) | (2 * 13 - 1);
    static const uint32_t r1[13] = { 0x244, 0x034, 0x030, 0x038, 0x03C, 0x168, 0x140, 0x110, 0x1C0, 0x1C4, 0x1C8, 0x180, 0x2B4 };
    uint32_t v1[13] = {
        masked(1u << 3, 1) | masked(1u << 0, inhibit),   /* CONTEXT_CONTROL */
        0, c->tail, c->ring.ggtt, 0x1,                /* head, tail, start, ctl: 4 KB, valid */
        0, 0, 1u << 5, 0, 0, 0, 0, 0 };
    for (int k = 0; k < 13; k++) { r[i++] = base + r1[k]; r[i++] = v1[k]; }
    for (int k = 0; k < 5; k++) r[i++] = 0;
    r[i++] = 0x11000000 | (1u << 12) | (2 * 9 - 1);
    static const uint32_t r2[9] = { 0x3A8, 0x28C, 0x288, 0x284, 0x280, 0x27C, 0x278, 0x274, 0x270 };
    uint32_t v2[9] = { 0, 0, 0, 0, 0, 0, 0, (uint32_t)(c->pml4 >> 32), (uint32_t)c->pml4 };
    for (int k = 0; k < 9; k++) { r[i++] = base + r2[k]; r[i++] = v2[k]; }
    r[i++] = 0x05000000 | 1;
}

static bool ctx_create(GpuCtx *c, Engine *e, uint64_t pml4)
{
    memset(c, 0, sizeof *c);
    c->e = e;
    c->pml4 = pml4;
    static uint32_t next_swid;
    c->swid = 1 + (next_swid++ % 2047);               /* 11 bits, 0 left out */
    /* the render state is big: 32 pages leave the engine's save room */
    if (!gpu_mem(&c->img, e->base == RCS ? 32 : 4) || !gpu_mem(&c->ring, 1)) return false;
    ctx_regs(c, true);
    return true;
}

/* commands into the ring, then the request's seqno stored into the
   engine's status page; submitted. The seqno back (0: failed). */
static uint32_t ctx_submit(GpuCtx *c, const uint32_t *cmds, uint32_t n)
{
    Engine *e = c->e;
    volatile uint32_t *ring = c->ring.v;
    uint32_t need = (n + 30) * 4;
    if (c->tail + need > 4096) {                      /* not at the end: from the start (waited for before) */
        while (c->tail < 4096) { ring[c->tail / 4] = 0; c->tail += 4; }
        c->tail = 0;
    }
    uint32_t t = c->tail / 4;
    if (c->stamp) {                                   /* MI_STORE_REGISTER_MEM, GGTT: the timestamp */
        ring[t++] = 0x12000002 | (1u << 22);
        ring[t++] = e->base + 0x358;
        ring[t++] = e->hwsp.ggtt + 0x480; ring[t++] = 0;
    }
    if (c->pml4) {                                    /* the PPGTT root (a restore-inhibited context lacks it) */
        ring[t++] = 0x11000000 | (2 * 2 - 1);
        ring[t++] = e->base + 0x274; ring[t++] = (uint32_t)(c->pml4 >> 32);
        ring[t++] = e->base + 0x270; ring[t++] = (uint32_t)c->pml4;
    }
    if (c->inval) {                                   /* MI_FLUSH_DW: invalidate the TLB (a post-sync write with it) */
        ring[t++] = 0x13000002 | (1u << 18) | (1u << 14);
        ring[t++] = (e->hwsp.ggtt + 0x800) | (1u << 2); ring[t++] = 0; ring[t++] = 0;
    }
    for (uint32_t k = 0; k < n; k++) ring[t++] = cmds[k];
    if (c->stamp) {
        ring[t++] = 0x12000002 | (1u << 22);
        ring[t++] = e->base + 0x358;
        ring[t++] = e->hwsp.ggtt + 0x484; ring[t++] = 0;
    }
    uint32_t seq = ++e->seqno;
    ring[t++] = 0x10400002;                           /* MI_STORE_DATA_IMM, GGTT: the seqno */
    ring[t++] = e->hwsp.ggtt + 0x400;
    ring[t++] = 0;
    ring[t++] = seq;
    ring[t++] = 0x01000000;                           /* MI_USER_INTERRUPT: "done" to the CPU (i915_irq) */
    if (t & 1) ring[t++] = 0;                         /* a whole number of qwords */
    c->tail = (t * 4) % 4096;
    if (!c->ran) {
        ctx_regs(c, true);
    } else {
        /* the engine's saved state stays (its head among it); only the
           new tail, and the context control restoring, go into the image */
        volatile uint32_t *r = c->img.v + 1024;
        flush(r, 64);
        r[3] = masked(1u << 3, 1) | masked(1u << 0, 0);
        r[7] = c->tail;
    }
    flush(c->ring.v, 4096);
    flush(c->img.v + 1024, 4096);
    uint64_t desc = (uint64_t)c->img.ggtt | 1u | ((c->pml4 ? 3u : 1u) << 3) | (1u << 8) |
                    ((uint64_t)c->swid << 37);
    wr(e->base + 0x518, 0);                           /* ELSQ port 1: empty */
    wr(e->base + 0x51C, 0);
    wr(e->base + 0x510, (uint32_t)desc);
    wr(e->base + 0x514, (uint32_t)(desc >> 32));
    wr(e->base + 0x550, 1);                           /* EL_CTRL_LOAD */
    c->ran = true;
    return seq;
}

static bool wait_seqno(Engine *e, uint32_t seq, uint32_t ms)
{
    volatile uint32_t *slot = e->hwsp.v + 0x100;
    extern uint64_t sched_clock_ns(void);
    extern bool bkl_drop(void);
    extern void bkl_retake(bool had);
    extern void sched_yield(void);
    if (g_irq_on) {
        /* Linux's way: the batch ends with MI_USER_INTERRUPT, i915_irq()
           wakes the engine's queue -- asleep meanwhile, the CPU free. (A
           re-check every 10 ms in case one is missed.) */
        WaitQueue *q = &g_eng_wq[e == &g_bcs];
        uint64_t t0 = timer_get_ms();
        for (;;) {
            uint64_t wseq = waitqueue_seq();
            flush(slot, 64);
            if ((int32_t)(*slot - seq) >= 0) return true;
            if (timer_get_ms() - t0 > ms) return false;
            waitqueue_wait_ms_seq(q, timer_get_ms() + 10, wseq);
        }
    }
    /* No interrupts (before they are set up, or no MSI): watched closely
       for up to 100 ms with the big kernel lock let go and the core given
       up every millisecond (each yield moves the thread's EEVDF deadline a
       slice on: no oftener), then a sleep a turn. */
    uint64_t s0 = sched_clock_ns(), yielded = s0;
    bool had = bkl_drop();
    bool done = false;
    for (;;) {
        uint64_t now = sched_clock_ns();
        if (now - s0 >= 100000000ull) break;
        flush(slot, 64);
        if ((int32_t)(*slot - seq) >= 0) { done = true; break; }
        __asm__ volatile("pause");
        if (now - yielded >= 1000000ull) { sched_yield(); yielded = now; }
    }
    bkl_retake(had);
    if (done) return true;
    uint64_t t0 = timer_get_ms();
    for (;;) {
        flush(slot, 64);
        if ((int32_t)(*slot - seq) >= 0) return true;
        if (timer_get_ms() - t0 > ms) return false;
        sched_sleep_ms(1);
    }
}

/* ---- per-process GPU address spaces (PPGTT, 4 levels) ------------------ */


static bool vm_create(GpuVm *vm)
{
    return gpu_mem(&vm->pml4, 1);
}

/* the table one level down at `t[i]`: made when missing */
static volatile uint64_t *vm_next(volatile uint64_t *t, uint32_t i)
{
    if (!(t[i] & 1)) {
        void *p = pmm_alloc_contiguous(1);
        if (!p) return NULL;
        memset(P2V((uint64_t)(uintptr_t)p), 0, 4096);
        t[i] = (uint64_t)(uintptr_t)p | 3;            /* present | writable */
        flush(&t[i], 8);
    }
    volatile uint64_t *n = (volatile uint64_t *)P2V(t[i] & 0x7FFFFFF000ull);
    return n;
}

/* ---- MOCS: how each kind of access is cached, by index ------------------
   Mesa names an index in its surface states and commands (2 for its own
   data, 61 for the screen's buffers), and the hardware uses some by itself
   (60 for compression data): Linux's TGL table (intel_mocs.c, tgl_mocs_table
   with the Gen11 entries), unlisted ones as the page table says. (The firmware had
   left a table of its own; HiZ and CCS were broken either way.) */

#define LE_UC   1u                                    /* LE_CACHEABILITY: uncached */
#define LE_WB   3u                                    /*                  write-back */
#define LE_LLC  (1u << 2)                             /* LE_TGT_CACHE: LLC */
#define LE_LRUM(v) ((uint32_t)(v) << 4)
#define LE_AOM(v)  ((uint32_t)(v) << 6)
#define LE_RSC(v)  ((uint32_t)(v) << 7)
#define LE_SCC(v)  ((uint32_t)(v) << 8)
#define LE_SSE(v)  ((uint32_t)(v) << 17)
#define L3_UC   (1u << 4)
#define L3_WB   (3u << 4)

static void mocs_init(void)
{
    uint32_t le[64], l3[64];
    static const struct { uint8_t i; uint32_t le; uint16_t l3; } t[] = {
        { 2,  LE_WB | LE_LLC | LE_LRUM(3), L3_WB },                       /* base: L3 + LLC */
        { 3,  LE_UC | LE_LLC, L3_UC },                                    /* base: uncached */
        { 4,  LE_UC | LE_LLC, L3_WB },                                    /* base: L3 */
        { 5,  LE_WB | LE_LLC | LE_LRUM(3), L3_UC },                       /* base: LLC */
        { 6,  LE_WB | LE_LLC | LE_LRUM(1), L3_UC },
        { 7,  LE_WB | LE_LLC | LE_LRUM(1), L3_WB },
        { 8,  LE_WB | LE_LLC | LE_LRUM(2), L3_UC },
        { 9,  LE_WB | LE_LLC | LE_LRUM(2), L3_WB },
        { 10, LE_WB | LE_LLC | LE_LRUM(3) | LE_AOM(1), L3_UC },
        { 11, LE_WB | LE_LLC | LE_LRUM(3) | LE_AOM(1), L3_WB },
        { 12, LE_WB | LE_LLC | LE_LRUM(1) | LE_AOM(1), L3_UC },
        { 13, LE_WB | LE_LLC | LE_LRUM(1) | LE_AOM(1), L3_WB },
        { 14, LE_WB | LE_LLC | LE_LRUM(2) | LE_AOM(1), L3_UC },
        { 15, LE_WB | LE_LLC | LE_LRUM(2) | LE_AOM(1), L3_WB },
        { 18, LE_WB | LE_LLC | LE_LRUM(3) | LE_SSE(3), L3_WB },           /* self-snoop */
        { 19, LE_WB | LE_LLC | LE_LRUM(3) | LE_SCC(7), L3_WB },           /* skip caching */
        { 20, LE_WB | LE_LLC | LE_LRUM(3) | LE_SCC(3), L3_WB },
        { 21, LE_WB | LE_LLC | LE_LRUM(3) | LE_SCC(1), L3_WB },
        { 22, LE_WB | LE_LLC | LE_LRUM(3) | LE_RSC(1) | LE_SCC(3), L3_WB },
        { 23, LE_WB | LE_LLC | LE_LRUM(3) | LE_RSC(1) | LE_SCC(7), L3_WB },
        { 48, LE_WB | LE_LLC | LE_LRUM(3), L3_WB },                       /* L1 implicitly on */
        { 49, LE_UC | LE_LLC, L3_WB },
        { 50, LE_WB | LE_LLC | LE_LRUM(3), L3_UC },
        { 51, LE_UC | LE_LLC, L3_UC },
        { 60, LE_WB | LE_LLC | LE_LRUM(3), L3_UC },                       /* the hardware's: CCS */
        { 61, LE_UC | LE_LLC, L3_WB },                                    /* the hardware's: displayable */
        { 62, LE_WB | LE_LLC | LE_LRUM(3), L3_UC },                       /* reserved */
        { 63, LE_WB | LE_LLC | LE_LRUM(3), L3_UC },
    };
    for (int i = 0; i < 64; i++) { le[i] = 0; l3[i] = L3_UC; }   /* LE 0: as the PTE says */
    for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++) { le[t[k].i] = t[k].le; l3[t[k].i] = t[k].l3; }
    for (int i = 0; i < 64; i++) wr(0x4000 + 4 * i, le[i]);                     /* GEN12_GLOBAL_MOCS */
    for (int i = 0; i < 32; i++) wr(0xB020 + 4 * i, l3[2 * i] | (l3[2 * i + 1] << 16));   /* LNCFCMOCS */
}

/* ---- the render engine's workarounds (Linux: intel_workarounds.c, TGL) ---
   Without them the engine hung on push constants (3DSTATE_CONSTANT_*)
   after a few seconds of Qt's drawing, and after a reset on everything.
   Masked registers: the bit's mask in the high half. Again after a reset. */

#define MASKED_EN(b) (((uint32_t)(b) << 16) | (uint32_t)(b))

static void rcs_workarounds(void)
{
    wr(0x20EC, MASKED_EN(1u << 1));                   /* Wa_1606700617: CS_DEBUG_MODE1 FF_DOP_CLOCK_GATE_DISABLE */
    wr(0xE4F4, MASKED_EN((1u << 8) | (1u << 14)));    /* Wa_1409804808 PUSH_CONST_DEREF_HOLD_DIS,
                                                         Wa_1606931601 DISABLE_EARLY_READ: ROW_CHICKEN2 */
    wr(0xE48C, MASKED_EN(1u << 9));                   /* Wa_14010229206: ROW_CHICKEN4 DISABLE_TDL_PUSH */
    wr(0xE18C, MASKED_EN(1u << 15));                  /* Wa_1406941453: SAMPLER_MODE ENABLE_SMALLPL */
    wr(RCS + 0x50, MASKED_EN((1u << 7) | (1u << 12)));/* Wa_1607030317 & co: PSMI_CTL */
    wr(0x20A0, rd(0x20A0) | (1u << 19));              /* Wa_1407928979 & co: FF_THREAD_MODE TESSELLATION_DOP_GATE_DISABLE */
    wr(0x7000, MASKED_EN(1u << 15));                  /* CACHE_MODE_0: DISABLE_REPACKING_FOR_COMPRESSION */
    /* registers Mesa's own batches write (Linux's tgl_whitelist_build):
       an unprivileged batch's LRI to any other is dropped -- Mesa's HiZ and
       compression workarounds among them. RING_FORCE_TO_NONPRIV, 12 slots;
       the rest name a harmless register (RING_NOPID), as Linux does. */
    static const uint32_t wl[] = {
        0x2348 | (1u << 28) | 1u,                     /* PS_INVOCATION_COUNT: read, 4 registers */
        0x7010,                                       /* COMMON_SLICE_CHICKEN1 (Wa_1508744258, Wa_1808121037) */
        0x7018,                                       /* HIZ_CHICKEN (Wa_1806527549) */
        0x7304,                                       /* COMMON_SLICE_CHICKEN3 (tuning) */
    };
    for (uint32_t i = 0; i < 12; i++)
        wr(RCS + 0x4D0 + 4 * i, i < sizeof wl / sizeof wl[0] ? wl[i] : RCS + 0x94);
}

/* ---- the GPU's clock: as fast as it goes --------------------------------
   Software requests only (RPNSWREQ), stepped -- see below. boot.cfg
   gpuclock=0 leaves the firmware's clock.
   Left alone it ran at whatever the firmware set -- the lowest: a login
   screen frame took 150 ms. Software mode (no autonomous up/down), the
   request at RP0, the fastest (RP_STATE_CAP in 50 MHz units, RPNSWREQ in
   16.67 MHz ones at bit 23). */
static void gt_clock_up(void)
{
    uint32_t cap = rd(0x145998);                      /* GEN6_RP_STATE_CAP (MCHBAR mirror) */
    uint32_t rp0 = cap & 0xFF, rpn = (cap >> 16) & 0xFF;
    uint32_t cur = rd(0xA008) >> 23;                  /* RPNSWREQ: the firmware's request */
    say("clock: before, MHz ", ((rd(0x1381B4) >> 11) & 0x1FF) * 50 / 3);   /* GEN12_RPSTAT1 */
    say("clock: the fastest (RP0), MHz ", rp0 * 50);
    say("clock: the slowest (RPn), MHz ", rpn * 50);
    if (!rp0) return;
    /* up 50 MHz a step, 10 ms each: one jump from the firmware's 200 MHz
       to RP0 stopped the whole machine (tools/hw/gpuclock.c found it; the
       steps went through) */
    for (uint32_t f = cur + 3; f <= rp0 * 3; f += 3) {
        wr(0xA008, f << 23);                          /* RPNSWREQ, 16.67 MHz units */
        sched_sleep_ms(10);                           /* (a thread: asleep, not spinning) */
    }
    say("clock: now, MHz ", ((rd(0x1381B4) >> 11) & 0x1FF) * 50 / 3);
}

/* the per-context ones: registers the context image holds, so LRIs at the
   start of every render request (i915_uapi.c execbuffer) */
uint32_t i915_rcs_ctx_workarounds(uint32_t *cs)
{
    uint32_t n = 0;
    cs[n++] = 0x11000005;                             /* MI_LOAD_REGISTER_IMM, 3 registers */
    cs[n++] = 0x7010; cs[n++] = MASKED_EN(1u << 14);  /* Wa_1508744258: COMMON_SLICE_CHICKEN1 RHWO_OPTIMIZATION_DISABLE */
    cs[n++] = 0x7304; cs[n++] = MASKED_EN(1u << 9);   /* Wa_1409142259 & co: COMMON_SLICE_CHICKEN3 DISABLE_CPS_AWARE_COLOR_PIPE */
    cs[n++] = 0x6604; cs[n++] = (224u << 24) | (4u << 16);   /* Wa_16011163337, Wa_1604555607: FF_MODE2 GS timer 224, TDS 128 */
    return n;
}

/* `pages` pages from `phys` at GPU address `va`; pat: the PAT index (0 WB,
   3 UC: PWT | PCD) */
static bool vm_map(GpuVm *vm, uint64_t va, uint64_t phys, uint32_t pages, uint32_t pat)
{
    uint64_t bits = 3 | ((pat & 1) ? (1u << 3) : 0) | ((pat & 2) ? (1u << 4) : 0) | ((pat & 4) ? (1u << 7) : 0);
    for (uint32_t k = 0; k < pages; k++, va += 4096, phys += 4096) {
        volatile uint64_t *pdp = vm_next((volatile uint64_t *)vm->pml4.v, (va >> 39) & 511);
        volatile uint64_t *pd = pdp ? vm_next(pdp, (va >> 30) & 511) : NULL;
        volatile uint64_t *pt = pd ? vm_next(pd, (va >> 21) & 511) : NULL;
        if (!pt) return false;
        pt[(va >> 12) & 511] = phys | bits;
        flush(&pt[(va >> 12) & 511], 8);
    }
    return true;
}

/* ---- an engine stuck: reset it (GDRST) -------------------------------- */

/* the GT-wide setup a full reset loses: cache types, MOCS, workarounds,
   the clock */
/* bring-up steps named on the stick's log at once (kernel/sticklog.c): a
   hang that stops the whole machine leaves no later line -- the last one
   is the step that hung */
static void step(const char *what)
{
    extern void sticklog_panic_flush(void);
    serial_write_string("i915: step: ");
    serial_write_string(what);
    serial_write_string("\r\n");
    sticklog_panic_flush();
}

static void gt_setup(void)
{
    step("PAT");
    static const uint32_t pat[8] = { 3, 1, 2, 0, 3, 3, 3, 3 };   /* 0 WB, 1 WC, 2 WT, 3 UC */
    for (int k = 0; k < 8; k++) wr(0x4800 + 4 * k, pat[k]);
    step("MOCS");
    mocs_init();
    step("render workarounds, whitelist");
    rcs_workarounds();
    if (g_clock) {
        step("clock");
        gt_clock_up();
    }
    step("GT set up");
}

/* An engine reset as Linux's intel_reset.c does it: forcewake held all
   through (its writes were lost when the caller had let go); the command
   streamer stopped first (MI_MODE STOP_RING, then MODE_IDLE) with the
   Gen12 prefetch off (Wa_22011802037); then RING_RESET_CTL's
   ready-to-reset handshake. An engine that never gets ready -- a memory
   read still out -- gets a full GT reset instead: after a plain engine
   reset in that state every later batch hung. */
static void engine_reset(Engine *e)
{
    fw_get();
    wr(e->base + 0x29C, masked(1u << 10, 1));         /* RING_MODE: GEN12_GFX_PREFETCH_DISABLE */
    wr(e->base + 0x9C, masked(1u << 8, 1));           /* MI_MODE: STOP_RING */
    uint64_t t0 = timer_get_ms();
    while (!(rd(e->base + 0x9C) & (1u << 9)) && timer_get_ms() - t0 < 50) ;   /* MODE_IDLE */
    wr(e->base + 0xD0, masked(1u, 1));                /* RING_RESET_CTL: request */
    t0 = timer_get_ms();
    while (!(rd(e->base + 0xD0) & 2) && timer_get_ms() - t0 < 700) ;
    bool ready = rd(e->base + 0xD0) & 2;
    uint32_t bit = !ready ? 1u : e->base == RCS ? (1u << 1) : (1u << 2);   /* GRDOM_FULL / _RENDER / _BLT */
    if (!ready) say("engine reset: not ready, a full GT reset; RESET_CTL ", rd(e->base + 0xD0));
    wr(0x941C, bit);
    t0 = timer_get_ms();
    while ((rd(0x941C) & bit) && timer_get_ms() - t0 < 500) ;
    say("reset, GDRST now ", rd(0x941C));
    wr(e->base + 0xD0, masked(1u, 0));                /* ready-to-reset off again */
    if (!ready) {
        gt_setup();
        if (g_irq_on) { irq_gt_enable(); wr(0x190010, 1u << 31); }
        g_bcs.up = g_rcs.up = false;
        engine_init(&g_rcs);
        engine_init(&g_bcs);
    } else {
        if (e->base == RCS) { mocs_init(); rcs_workarounds(); }
        e->up = false;
        engine_init(e);                               /* its execlists and status page again */
    }
    fw_put();
}

/* ---- the self-test: two requests on one context per engine ------------ */

static bool selftest(Engine *e)
{
    GpuCtx c;
    GpuMem target;
    if (!ctx_create(&c, e, 0) || !gpu_mem(&target, 1)) return false;
    for (int round = 0; round < 2; round++) {
        uint32_t want = 0xC0DE0000u | (uint32_t)(round << 8) | (e->base == RCS ? 0x11 : 0x22);
        uint32_t cmds[4] = { 0x10400002, target.ggtt + (uint32_t)round * 4, 0, want };
        uint32_t seq = ctx_submit(&c, cmds, 4);
        if (!wait_seqno(e, seq, 1000)) {
            say(round ? "self-test: the second request (restoring the context) never ended, engine " :
                        "self-test: the first request never ended, engine ", e->base);
            say("  execlists status ", rd(e->base + 0x234));
            say("  head/tail ", ((uint64_t)rd(e->base + 0x34) << 32) | rd(e->base + 0x30));
            say("  ipehr ", rd(e->base + 0x68));
            engine_reset(e);
            return false;
        }
        flush(target.v, 64);
        if (target.v[round] != want) { say("self-test: the store did not land, got ", target.v[round]); return false; }
    }
    serial_write_string("i915: self-test passed on ");
    serial_write_string(e->name);
    serial_write_string(": two requests on one context, the second restoring the first's state\r\n");
    return true;
}

/* A batch through a PPGTT on the blitter, timed: user-space tests took
   ~198 ms for such a request where a GGTT one took 0.2 ms. Which part? */
static bool selftest_ppgtt(Engine *e, bool inval)
{
    GpuVm vm;
    GpuMem bt;                                        /* page 0 the batch, page 1 the target */
    GpuCtx c;
    if (!vm_create(&vm) || !gpu_mem(&bt, 2) || !vm_map(&vm, 0x10000000, bt.phys, 2, 0)) return false;
    uint32_t want = inval ? 0x7A9E0002u : 0x7A9E0001u;
    volatile uint32_t *b = bt.v;
    b[0] = 0x10000002; b[1] = 0x10001000; b[2] = 0; b[3] = want;     /* MI_STORE_DATA_IMM, PPGTT */
    b[4] = 0x13000002; b[5] = 0; b[6] = 0; b[7] = 0;                  /* MI_FLUSH_DW */
    b[8] = 0x05000000; b[9] = 0;                                      /* MI_BATCH_BUFFER_END */
    flush(bt.v, 8192);
    if (!ctx_create(&c, e, vm.pml4.phys)) return false;
    c.inval = inval;
    c.stamp = true;
    uint32_t cmds[4] = { 0x18800101, 0x10000000, 0, 0 };              /* MI_BATCH_BUFFER_START, PPGTT */
    uint64_t t0 = timer_get_ms();
    uint32_t seq = ctx_submit(&c, cmds, 4);
    bool ok = wait_seqno(e, seq, 2000);
    uint64_t ms = timer_get_ms() - t0;
    flush(bt.v + 1024, 64);
    flush(e->hwsp.v + 0x120, 64);
    uint32_t ticks = e->hwsp.v[0x121] - e->hwsp.v[0x120];
    say(inval ? "PPGTT batch with a TLB invalidate: ms " : "PPGTT batch, no TLB invalidate: ms ", ms);
    say("  the GPU's own ticks between start and end ", ticks);
    if (!ok || bt.v[1024] != want) {
        say("  FAILED: target ", bt.v[1024]);
        if (!ok) engine_reset(e);
        return false;
    }
    return true;
}

/* ---- probe ------------------------------------------------------------ */

/* ---- interrupts (Gen11+, as Linux's gen11_irq_handler) -------------------
   MSI to the boot core's vector VEC_GPU (kernel/irq.c sends it here); the
   GT's "user interrupt" (MI_USER_INTERRUPT at each request's end) of the
   render and copy engines on; display interrupts off (polled). */

#define GFX_MSTR_IRQ          0x190010                /* bit 31 master enable; 0 GT DW0, 16 display */
#define GT_INTR_DW0           0x190018                /* a bit an engine: 0 RCS0, 1 BCS0 */
#define INTR_IDENTITY_REG0    0x190060                /* bit 31 valid; 15:0 the engine's bits */
#define IIR_REG_SELECTOR0     0x190070
#define RENDER_COPY_INTR_EN   0x190030                /* render << 16 | copy */
#define RCS0_RSVD_INTR_MASK   0x190090                /* 1: masked; render in 31:16 */
#define BCS_RSVD_INTR_MASK    0x1900A0                /* copy in 31:16 */
#define DISPLAY_INT_CTL       0x44200
#define USER_INTERRUPT        (1u << 0)

/* the GT's side: user interrupts of RCS0 and BCS0, display off (again
   after a full GT reset, which forgets them) */
static void irq_gt_enable(void)
{
    wr(GFX_MSTR_IRQ, 0);
    wr(DISPLAY_INT_CTL, 0);
    wr(RENDER_COPY_INTR_EN, (USER_INTERRUPT << 16) | USER_INTERRUPT);
    wr(RCS0_RSVD_INTR_MASK, ~(USER_INTERRUPT << 16));
    wr(BCS_RSVD_INTR_MASK, ~(USER_INTERRUPT << 16));
    wr(GT_INTR_DW0, ~0u);                             /* (anything left: cleared) */
}

void i915_irq(void)
{
    if (!g_irq_on) return;
    wr(GFX_MSTR_IRQ, 0);                              /* off while handled */
    uint32_t master = rd(GFX_MSTR_IRQ);
    if (master & 1) {
        uint32_t dw = rd(GT_INTR_DW0);
        for (uint32_t b = 0; b < 2; b++) {
            if (!(dw & (1u << b))) continue;
            wr(IIR_REG_SELECTOR0, 1u << b);
            uint32_t ident = 0;
            for (int n = 0; n < 200 && !((ident = rd(INTR_IDENTITY_REG0)) & (1u << 31)); n++) ;
            wr(INTR_IDENTITY_REG0, ident);            /* (acknowledged) */
            if (ident & USER_INTERRUPT) waitqueue_wake_all(&g_eng_wq[b]);
        }
        wr(GT_INTR_DW0, dw);
    }
    g_irqs++;
    wr(GFX_MSTR_IRQ, 1u << 31);
}

static void irq_setup(void)
{
    if (g_dev < 0) return;
    /* the MSI capability */
    uint8_t cap = pci_read_config(0, (uint8_t)g_dev, 0, 0x34) & 0xFC;
    uint32_t h = 0;
    while (cap) {
        h = pci_read_config(0, (uint8_t)g_dev, 0, cap);
        if ((h & 0xFF) == 0x05) break;
        cap = (h >> 8) & 0xFC;
    }
    if (!cap) { say("no MSI capability: requests waited for by polling, ", 0); return; }
    uint16_t ctl = (uint16_t)(h >> 16);
    bool is64 = ctl & (1u << 7);
    uint32_t apic = g_cpus[0].apic_id;                /* the boot core */
    pci_write_config(0, (uint8_t)g_dev, 0, cap + 4, 0xFEE00000u | (apic << 12));
    if (is64) {
        pci_write_config(0, (uint8_t)g_dev, 0, cap + 8, 0);
        pci_write_config(0, (uint8_t)g_dev, 0, cap + 12, VEC_GPU);
    } else {
        pci_write_config(0, (uint8_t)g_dev, 0, cap + 8, VEC_GPU);
    }
    irq_gt_enable();
    ctl = (uint16_t)((ctl & ~(7u << 4)) | 1u);        /* one message, enabled */
    pci_write_config(0, (uint8_t)g_dev, 0, cap, (h & 0xFFFF) | ((uint32_t)ctl << 16));
    uint32_t cmd = pci_read_config(0, (uint8_t)g_dev, 0, 0x04);
    pci_write_config(0, (uint8_t)g_dev, 0, 0x04, cmd | (1u << 10));   /* INTx off */
    g_irq_on = true;
    wr(GFX_MSTR_IRQ, 1u << 31);
    say("interrupts: MSI to the boot core, vector ", VEC_GPU);
}

/* (the log, a while after: did they come?) */
uint64_t i915_irq_count(void) { return g_irqs; }

static void i915_thread(void *arg)
{
    (void)arg;
    int dev = -1;
    for (int d = 0; d < 32; d++) {
        uint32_t id = pci_read_config(0, (uint8_t)d, 0, 0);
        if ((id & 0xFFFF) == 0x8086 && (pci_read_config(0, (uint8_t)d, 0, 8) >> 24) == 0x03) { dev = d; break; }
    }
    if (dev < 0) { serial_write_string("i915: no Intel GPU\r\n"); return; }
    uint32_t id = pci_read_config(0, (uint8_t)dev, 0, 0) >> 16;
    g_devid = id;
    g_dev = dev;
    g_rev = pci_read_config(0, (uint8_t)dev, 0, 8) & 0xFF;
    g_subsys = pci_read_config(0, (uint8_t)dev, 0, 0x2C);
    say("Intel GPU, device ", id);
    /* Gen12 integrated (Tiger Lake 9Axx, Rocket/Alder Lake parts later) */
    if ((id & 0xFF00) != 0x9A00) { say("not a Tiger Lake GPU: left alone, device ", id); return; }
    uint32_t lo = pci_read_config(0, (uint8_t)dev, 0, 0x10), hi = pci_read_config(0, (uint8_t)dev, 0, 0x14);
    uint64_t bar0 = ((uint64_t)hi << 32) | (lo & ~0xFull);
    uint32_t cmd = pci_read_config(0, (uint8_t)dev, 0, 0x04);
    pci_write_config(0, (uint8_t)dev, 0, 0x04, cmd | 0x6);   /* memory + bus master */
    g_mmio = (volatile uint8_t *)vmm_map_mmio(bar0, 16u << 20);
    if (!g_mmio) { serial_write_string("i915: BAR0 not mapped\r\n"); return; }
    g_gtt = (volatile uint64_t *)(g_mmio + (8u << 20));
    uint32_t ggms = (pci_read_config(0, (uint8_t)dev, 0, 0x50) >> 6) & 3;
    g_gtt_entries = ggms ? ((1ull << ggms) << 20) / 8 : 0;
    while (g_gtt_fw < g_gtt_entries && (g_gtt[g_gtt_fw] & 1) &&
           (g_gtt_fw == 0 || (g_gtt[g_gtt_fw] & ~0xFFFull) == (g_gtt[g_gtt_fw - 1] & ~0xFFFull) + 4096))
        g_gtt_fw++;
    g_gtt_next = (g_gtt_fw + 256 + 255) & ~255ull;    /* 1 MB past the firmware's */
    say("GGTT entries ", g_gtt_entries);
    say("  the firmware's framebuffer: entries up to ", g_gtt_fw);
    spin_init(&g_lock, "i915");
    step("forcewake");
    if (!fw_get()) { serial_write_string("i915: forcewake: no ack\r\n"); return; }
    gt_setup();
    step("engines");
    if (!engine_init(&g_rcs) || !engine_init(&g_bcs)) { serial_write_string("i915: engines not set up\r\n"); fw_put(); return; }
    g_up = true;
    step("self-test: blitter");
    bool ok = selftest(&g_bcs);
    step("self-test: render");
    ok &= selftest(&g_rcs);
    step("self-test: PPGTT");
    ok &= selftest_ppgtt(&g_bcs, false);
    ok &= selftest_ppgtt(&g_bcs, true);
    step("done");
    irq_setup();
    extern void i915_uapi_start(void);
    i915_uapi_start();                                /* the engines' queue threads: the kernel's, no process's */
    fw_put();
    serial_write_string(ok ? "i915: ready (render-only)\r\n" : "i915: SELF-TEST FAILED\r\n");
}

/* boot.cfg "gpu=1": the driver comes up (off by default while it grows) */
static bool cfg_has(const char *cfg, const char *line)
{
    for (int i = 0; cfg[i]; i++) {
        if (i && cfg[i - 1] != '\n') continue;
        int k = 0;
        while (line[k] && cfg[i + k] == line[k]) k++;
        if (!line[k] && (cfg[i + k] == '\n' || cfg[i + k] == '\r' || !cfg[i + k])) return true;
    }
    return false;
}

void i915_start(const char *cfg)
{
    g_clock = !cfg_has(cfg, "gpuclock=0");
    for (int i = 0; cfg[i]; i++) {
        if ((i == 0 || cfg[i - 1] == '\n') && cfg[i] == 'g' && cfg[i + 1] == 'p' && cfg[i + 2] == 'u' &&
            cfg[i + 3] == '=' && cfg[i + 4] == '1') {
            extern Thread *thread_create(void (*entry)(void *), void *arg);
            thread_create(i915_thread, NULL);
            return;
        }
    }
}

bool i915_ready(void) { return g_up; }

/* ---- for the uAPI (drivers/gpu/i915_uapi.c) --------------------------- */

uint32_t i915_device_id(void) { return g_devid; }
uint32_t i915_revision(void) { return g_rev; }
uint32_t i915_subsystem(void) { return g_subsys; }
uint32_t i915_rd(uint32_t reg) { return rd(reg); }
void     i915_wr(uint32_t reg, uint32_t v) { wr(reg, v); }
bool i915_fw_get(void) { return fw_get(); }
void i915_fw_put(void) { fw_put(); }
void i915_flush(volatile void *p, uint64_t n) { flush(p, n); }
bool i915_mem(GpuMem *m, uint32_t pages) { return gpu_mem(m, pages); }
bool i915_vm_create(GpuVm *vm) { return vm_create(vm); }
bool i915_vm_map(GpuVm *vm, uint64_t va, uint64_t phys, uint32_t pages, uint32_t pat) { return vm_map(vm, va, phys, pages, pat); }
bool i915_ctx_create(GpuCtx *c, Engine *e, uint64_t pml4) { return ctx_create(c, e, pml4); }
uint32_t i915_ctx_submit(GpuCtx *c, const uint32_t *cmds, uint32_t n) { return ctx_submit(c, cmds, n); }
bool i915_wait_seqno(Engine *e, uint32_t seq, uint32_t ms) { return wait_seqno(e, seq, ms); }
void i915_engine_reset(Engine *e) { engine_reset(e); }

/* the pages at `va` gone from the address space (their PTEs cleared) */
void i915_vm_unmap(GpuVm *vm, uint64_t va, uint32_t pages)
{
    for (uint32_t k = 0; k < pages; k++, va += 4096) {
        volatile uint64_t *t = (volatile uint64_t *)vm->pml4.v;
        static const int shift[3] = { 39, 30, 21 };
        int lvl = 0;
        for (; lvl < 3; lvl++) {
            uint64_t e = t[(va >> shift[lvl]) & 511];
            if (!(e & 1)) break;
            t = (volatile uint64_t *)P2V(e & 0x7FFFFFF000ull);
        }
        if (lvl < 3) continue;
        t[(va >> 12) & 511] = 0;
        flush(&t[(va >> 12) & 511], 8);
    }
}

/* the command streamer's timestamp rate: RPM_CONFIG0's crystal and its
   divider (Gen11+, as i915 reads it) */
uint32_t i915_cs_timestamp_hz(void)
{
    uint32_t c = rd(0x0D00);
    static const uint32_t xtal[4] = { 24000000, 19200000, 38400000, 25000000 };
    uint32_t f = xtal[(c >> 3) & 3];
    uint32_t shift = (c >> 1) & 3;
    return f >> (3 - shift);
}
