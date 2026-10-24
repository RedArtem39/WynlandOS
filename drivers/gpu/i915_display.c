/*
 * WynlandOS - the Intel display's primary plane and cursor, as the
 * firmware left them running (no modeset of our own): page flips without
 * a copy, and the pointer on the cursor plane.
 *
 * The firmware lit one pipe with its framebuffer behind GGTT offset 0
 * (drivers/gpu/i915.c keeps those entries). A flip here maps the frame's
 * object into the GGTT once (i915_ggtt_map), points the plane at it --
 * PLANE_STRIDE, the tiling in PLANE_CTL, then PLANE_SURF, whose write arms
 * the change for the next vblank -- and PLANE_SURFLIVE tells when the
 * display has moved over. Before, simpledrm copied every frame (8 MB) into
 * the firmware's framebuffer with the CPU, tearing as it went.
 *
 * Registers: Gen9+ universal planes (Linux: intel_display_reg_defs.h,
 * skl_universal_plane_regs.h), plane 1 of pipe p at 0x70180 + p * 0x1000,
 * the cursor at 0x70080 + p * 0x1000.
 */
#include <wynland/types.h>
#include <wynland/i915.h>
#include <wynland/memlayout.h>

extern void serial_write_string(const char *s);
extern void uint_to_hex(uint64_t v, char *buf);
extern void *memset(void *d, int c, size_t n);

#define PLANE_CTL(p)      (0x70180 + (p) * 0x1000)
#define PLANE_STRIDE(p)   (0x70188 + (p) * 0x1000)
#define PLANE_SIZE(p)     (0x70190 + (p) * 0x1000)
#define PLANE_SURF(p)     (0x7019C + (p) * 0x1000)
#define PLANE_OFFSET(p)   (0x701A4 + (p) * 0x1000)
#define PLANE_SURFLIVE(p) (0x701AC + (p) * 0x1000)
#define CUR_CTL(p)        (0x70080 + (p) * 0x1000)
#define CUR_BASE(p)       (0x70084 + (p) * 0x1000)
#define CUR_POS(p)        (0x70088 + (p) * 0x1000)
#define DE_PIPE_IMR(p)    (0x44404 + (p) * 0x10)
#define DE_PIPE_IIR(p)    (0x44408 + (p) * 0x10)
#define DE_PIPE_IER(p)    (0x4440C + (p) * 0x10)
#define PIPE_UNDERRUN     (1u << 31)
#define PIPE_PLANE1_FAULT (1u << 7)
#define PIPE_CURSOR_FAULT (1u << 11)
#define PIPE_WATCHED      (PIPE_UNDERRUN | PIPE_PLANE1_FAULT | PIPE_CURSOR_FAULT)
#define PIPE_FRMCOUNT(p)  (0x70040 + (p) * 0x1000)
#define PIPE_FLIPCOUNT(p) (0x70044 + (p) * 0x1000)
#define PLANE_BUF_CFG(p)  (0x7027C + (p) * 0x1000)
#define PLANE_WM0(p)      (0x70240 + (p) * 0x1000)
#define CUR_BUF_CFG(p)    (0x7017C + (p) * 0x1000)
#define CUR_WM(p, l)      (0x70140 + (p) * 0x1000 + (l) * 4)
#define CUR_WM_TRANS(p)   (0x70168 + (p) * 0x1000)
#define DE_PIPE_ISR(p)    (0x44400 + (p) * 0x10)
#define DC_STATE_EN       0x45504
#define PSR2_MAN_TRK_CTL  0x60910
/* panel self refresh: transcoder A's (pipe 0 on TGL; there is no EDP transcoder) */
#define EDP_PSR_CTL       0x60800
#define EDP_PSR_STATUS    0x60840
#define EDP_PSR2_CTL      0x60900
#define EDP_PSR2_STATUS   0x60940

#define PLANE_ENABLE      (1u << 31)
#define PLANE_TILING      (7u << 10)                  /* 0 linear, 1 X, 4 Y */
#define CUR_MODE_64_ARGB  0x27u

static void say(const char *s, uint64_t v)
{
    char b[20];
    serial_write_string("i915 display: ");
    serial_write_string(s);
    uint_to_hex(v, b);
    serial_write_string(b);
    serial_write_string("\r\n");
}

static int g_pipe = -1;                               /* the lit pipe; -2: looked, none usable */
static uint32_t g_ctl0, g_stride0, g_surf0, g_offset0;   /* the firmware's plane, to give back */
static uint32_t g_w, g_h;                             /* the plane's size (the mode) */
static bool g_ours;                                   /* the plane shows one of our objects */
static bool g_cursor_on;
static bool g_broken;                                 /* the display failed on our frames: copies from now on */
static uint32_t g_flips;
static uint32_t g_armed;                              /* the last surface armed */
static uint32_t g_missed, g_missed_run;               /* flips not taken by the next one (in a row) */
static GpuMem g_cursor;                               /* 64 x 64 ARGB */

/* scanned-out objects already in the GGTT: phys -> offset */
static struct { uint64_t phys; uint32_t pages, ggtt; } g_maps[16];

static bool find_pipe(void)
{
    if (g_pipe >= 0) return true;
    if (g_pipe == -2 || !i915_ready()) return false;
    for (int p = 0; p < 3; p++) {
        uint32_t ctl = i915_rd(PLANE_CTL(p));
        if (!(ctl & PLANE_ENABLE)) continue;
        g_ctl0 = ctl;
        g_stride0 = i915_rd(PLANE_STRIDE(p));
        g_surf0 = i915_rd(PLANE_SURF(p));
        g_offset0 = i915_rd(PLANE_OFFSET(p));
        uint32_t size = i915_rd(PLANE_SIZE(p));
        g_w = (size & 0xFFFF) + 1;
        g_h = (size >> 16) + 1;
        g_pipe = p;
        /* IIR latches only what IMR leaves unmasked; IER kept off for
           them (no interrupt: we poll) */
        i915_wr(DE_PIPE_IER(p), i915_rd(DE_PIPE_IER(p)) & ~PIPE_WATCHED);
        i915_wr(DE_PIPE_IMR(p), i915_rd(DE_PIPE_IMR(p)) & ~PIPE_WATCHED);
        i915_wr(DE_PIPE_IIR(p), ~0u);                    /* (write 1 to clear) */
        say("the firmware's plane: pipe ", (uint64_t)p);
        say("  PLANE_BUF_CFG ", i915_rd(PLANE_BUF_CFG(p)));
        say("  PLANE_WM level 0 ", i915_rd(PLANE_WM0(p)));
        say("  CUR_BUF_CFG ", i915_rd(CUR_BUF_CFG(p)));
        say("  PSR_CTL ", i915_rd(EDP_PSR_CTL));
        say("  PSR2_CTL ", i915_rd(EDP_PSR2_CTL));
        say("  PSR2_MAN_TRK_CTL ", i915_rd(PSR2_MAN_TRK_CTL));
        say("  DC_STATE_EN ", i915_rd(DC_STATE_EN));
        say("  PLANE_CTL ", ctl);
        say("  stride ", g_stride0);
        say("  surface ", g_surf0);
        say("  w<<16|h ", ((uint64_t)g_w << 16) | g_h);
        return true;
    }
    say("no lit plane found: frames are copied, ", 0);
    g_pipe = -2;
    return false;
}

static uint32_t ggtt_of(uint64_t phys, uint32_t pages)
{
    for (uint32_t i = 0; i < sizeof g_maps / sizeof g_maps[0]; i++)
        if (g_maps[i].phys == phys && g_maps[i].pages >= pages) return g_maps[i].ggtt;
    /* 256 KB aligned: what Linux asks of tiled scanout surfaces */
    uint32_t ggtt = i915_ggtt_map(phys, pages, 64);
    if (!ggtt) return 0;
    for (uint32_t i = 0; i < sizeof g_maps / sizeof g_maps[0]; i++) {
        if (g_maps[i].phys) continue;
        g_maps[i].phys = phys; g_maps[i].pages = pages; g_maps[i].ggtt = ggtt;
        break;
    }
    return ggtt;
}

/* Point the plane at an object: 0 if this frame can't be shown so (the
   caller copies it instead), else the GGTT offset the flip goes to. */
uint32_t i915_disp_flip(uint64_t phys, uint64_t size, uint32_t w, uint32_t h, uint32_t pitch, uint32_t tiling)
{
    if (g_broken || !find_pipe()) return 0;
    if (w != g_w || h != g_h || tiling > 1) return 0;
    uint32_t unit = tiling ? 512 : 64;                /* PLANE_STRIDE: in tile widths, or 64 bytes */
    if (pitch % unit || pitch / unit > 0x7FF) return 0;
    uint32_t ggtt = ggtt_of(phys, (uint32_t)((size + 4095) / 4096));
    if (!ggtt) return 0;
    /* (No CPU cache flush of the frame: Mesa writes scanout buffers
       uncached (MOCS), the CPU never touched it, and the batch's own end
       flushes the GPU's caches -- 8 MB of clflushopt a frame, in the
       syscall, bought nothing.) */
    int p = g_pipe;
    if (g_armed) {
        if ((i915_rd(PLANE_SURFLIVE(p)) & ~0xFFFu) != g_armed) { g_missed++; g_missed_run++; }
        else g_missed_run = 0;
        if (g_missed_run >= 8) {                      /* the plane follows no more: the copies */
            say("flips not taken 8 times in a row: back to copying; SURFLIVE ", i915_rd(PLANE_SURFLIVE(p)));
            i915_disp_restore();
            g_broken = true;
            return 0;
        }
    }
    i915_wr(PLANE_STRIDE(p), pitch / unit);
    i915_wr(PLANE_OFFSET(p), 0);
    i915_wr(PLANE_CTL(p), (g_ctl0 & ~PLANE_TILING) | (tiling ? (1u << 10) : 0));
    i915_wr(PLANE_SURF(p), ggtt);                     /* arms it: taken at the next vblank */
    g_armed = ggtt;
    g_ours = true;
    g_flips++;
    return ggtt;
}

/* After some flips: did the display take our frames? An underrun or a
   fault on the pipe, or the plane not following: the firmware's plane
   back, copies from now on (simpledrm). Logged either way. */
bool i915_disp_check(uint64_t phys, uint32_t pitch)
{
    (void)phys; (void)pitch;
    if (g_pipe < 0 || !g_ours || g_broken) return !g_broken;
    int p = g_pipe;
    uint32_t iir = i915_rd(DE_PIPE_IIR(p));
    say("check after flips ", g_flips);
    say("  flips not taken by the next ", g_missed);
    say("  pipe IIR (31 underrun, 7 plane fault, 11 cursor fault, 3 flip done, 0 vblank) ", iir);
    say("  pipe ISR ", i915_rd(DE_PIPE_ISR(p)));
    say("  frame counter ", i915_rd(PIPE_FRMCOUNT(p)));
    say("  flip counter ", i915_rd(PIPE_FLIPCOUNT(p)));
    say("  PLANE_CTL ", i915_rd(PLANE_CTL(p)));
    say("  PLANE_SURF ", i915_rd(PLANE_SURF(p)));
    say("  PLANE_SURFLIVE ", i915_rd(PLANE_SURFLIVE(p)));
    say("  PLANE_BUF_CFG ", i915_rd(PLANE_BUF_CFG(p)));
    say("  PSR_STATUS ", i915_rd(EDP_PSR_STATUS));
    say("  PSR2_STATUS ", i915_rd(EDP_PSR2_STATUS));
    i915_wr(DE_PIPE_IIR(p), ~0u);
    if (iir & PIPE_WATCHED) {
        say("the display failed on our frames: back to copying, IIR ", iir);
        i915_disp_restore();
        g_broken = true;
        return false;
    }
    return true;
}

/* the display shows `ggtt` now (the flip is done) */
bool i915_disp_flip_done(uint32_t ggtt)
{
    if (g_pipe < 0) return true;
    return (i915_rd(PLANE_SURFLIVE(g_pipe)) & ~0xFFFu) == ggtt;
}

/* the firmware's framebuffer back (the console), the cursor off */
void i915_disp_restore(void)
{
    if (g_pipe < 0 || !g_ours) return;
    int p = g_pipe;
    if (g_cursor_on) {
        i915_wr(CUR_CTL(p), 0);
        i915_wr(CUR_BASE(p), 0);
        g_cursor_on = false;
    }
    i915_wr(PLANE_STRIDE(p), g_stride0);
    i915_wr(PLANE_OFFSET(p), g_offset0);
    i915_wr(PLANE_CTL(p), g_ctl0);
    i915_wr(PLANE_SURF(p), g_surf0);
    g_ours = false;
}

/* the pointer's picture (ARGB, dim x dim, stride in pixels) onto the cursor
   plane, and on */
void i915_disp_cursor_image(const uint32_t *img, uint32_t dim, uint32_t stride)
{
    if (g_pipe < 0) return;
    if (!g_cursor.v && !i915_mem(&g_cursor, 4)) return;
    /* Its share of the display buffer (DDB) and watermarks first: the
       firmware left the cursor none, and enabled so the pipe took no
       update after it (the black screen). 32 blocks right after the
       primary plane's (PLANE_BUF_CFG: start 10:0, end 26:16, inclusive),
       level 0 only, as Linux's fixed cursor allocation. */
    uint32_t buf = i915_rd(PLANE_BUF_CFG(g_pipe));
    uint32_t pend = (buf >> 16) & 0x7FF;
    if (!g_cursor_on) {
        uint32_t cstart = pend + 1, cend = pend + 32;
        if (cend > 1023) { say("no display buffer room left for the cursor, plane ends at ", pend); return; }
        i915_wr(CUR_BUF_CFG(g_pipe), (cend << 16) | cstart);
        i915_wr(CUR_WM(g_pipe, 0), (1u << 31) | (4u << 14) | 16u);   /* enabled, 4 lines, 16 blocks */
        for (int l = 1; l < 8; l++) i915_wr(CUR_WM(g_pipe, l), 0);
        i915_wr(CUR_WM_TRANS(g_pipe), 0);
        say("cursor: display buffer blocks from ", cstart);
    }
    volatile uint32_t *d = g_cursor.v;
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++)
            d[y * 64 + x] = (img && x < dim && y < dim) ? img[y * stride + x] : 0;
    i915_flush(d, 64 * 64 * 4);                       /* the display reads memory, not the CPU's cache */
    int p = g_pipe;
    i915_wr(CUR_CTL(p), CUR_MODE_64_ARGB);
    i915_wr(CUR_BASE(p), g_cursor.ggtt);
    g_cursor_on = true;
}

/* the pointer to (x, y): from the mouse interrupt */
void i915_disp_cursor_move(int32_t x, int32_t y)
{
    if (g_pipe < 0 || !g_cursor_on) return;
    uint32_t pos = 0;
    if (x < 0) { pos |= 0x8000u; x = -x; }
    if (y < 0) { pos |= 0x80000000u; y = -y; }
    pos |= ((uint32_t)x & 0x7FFF) | (((uint32_t)y & 0x7FFF) << 16);
    int p = g_pipe;
    i915_wr(CUR_POS(p), pos);
    i915_wr(CUR_BASE(p), g_cursor.ggtt);              /* (the write that arms the update) */
}
