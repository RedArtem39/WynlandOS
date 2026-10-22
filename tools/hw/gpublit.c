/*
 * WynlandOS - gpublit: the GPU draws on the screen (tools/hw/gpulib.h).
 *
 * The firmware's framebuffer: its pages are what the GGTT's first entries
 * point to (pipe A scans out GGTT 0). They go into a PPGTT at 0x20000000,
 * uncached (PAT 3: the display engine reads memory, not the CPU's or the
 * GPU's caches); a batch on the blitter fills rectangles there with
 * XY_COLOR_BLT, then stores a number to say it got to the end.
 */
#include "gpulib.h"

#define MAGIC 0xB1171ED0u
#define VA    0x10000000ull        /* the batch, the done page */
#define FB_VA 0x20000000ull        /* the framebuffer */

int main(void)
{
    gpu_open();
    uint32_t w = (rd(0x6001C) >> 16) + 1, h = (rd(0x6001C) & 0xFFFF) + 1;
    uint32_t pitch = (rd(0x70188) & 0x7FF) * 64, surf = rd(0x7019C) & ~0xFFFu;
    uint32_t fb_pages = (pitch * h + 4095) / 4096;
    printf("screen %ux%u, pitch %u, surface GGTT %#x: %u pages\n", w, h, pitch, surf, fb_pages);
    if (!(rd(0x70180) >> 31) || pitch < w * 4 || fb_pages > 4 * 512) { printf("not the screen I expect: stopping\n"); return 1; }

    /* pages: 0 status, 1..4 context, 5 ring, 6 PML4, 7 PDP, 8 PD, 9..13 PTs (1 batch + 4 fb), 14 batch, 15 done */
    uint32_t n = 16;
    uint64_t phys;
    volatile uint32_t *mem = dma((uint64_t)n * 4096, &phys);
#define PAGE(k) (mem + (k) * 1024)
#define PHYS(k) (phys + (uint64_t)(k) * 4096)
    volatile uint32_t *regs = PAGE(2), *ring = PAGE(5), *batch = PAGE(14), *done = PAGE(15);
    volatile uint64_t *pml4 = (volatile uint64_t *)PAGE(6), *pdp = (volatile uint64_t *)PAGE(7), *pd = (volatile uint64_t *)PAGE(8);

    pml4[0] = PHYS(7) | 3;
    pdp[0] = PHYS(8) | 3;
    /* 0x10000000: PD 128 -> PT 9: batch, done (WB) */
    pd[(VA >> 21) & 511] = PHYS(9) | 3;
    volatile uint64_t *pt = (volatile uint64_t *)PAGE(9);
    pt[0] = PHYS(14) | 3;
    pt[1] = PHYS(15) | 3;
    /* 0x20000000: PD 256.. -> PTs 10..13: the framebuffer's pages, as the
       GGTT maps them, uncached (PAT index 3: PWT | PCD) */
    for (uint32_t k = 0; k < fb_pages; k++) {
        uint64_t pte = g_gtt[(surf >> 12) + k];
        if (!(pte & 1)) { printf("framebuffer page %u not in the GGTT: stopping\n", k); return 1; }
        volatile uint64_t *ptk = (volatile uint64_t *)PAGE(10 + k / 512);
        ptk[k % 512] = (pte & 0x7FFFFFF000ull) | 3 | (1u << 3) | (1u << 4);
    }
    for (uint32_t t = 0; t < (fb_pages + 511) / 512; t++) pd[((FB_VA >> 21) & 511) + t] = PHYS(10 + t) | 3;

    /* the batch: three rectangles (XY_COLOR_BLT, 32 bpp, PATCOPY), a
       flush, the done number */
    static const struct { uint32_t x, y, w, h, rgb; } r[3] = {
        { 80, 80, 360, 220, 0x00FF7A00 }, { 470, 80, 360, 220, 0x0000C8FF }, { 80, 330, 750, 120, 0x0040E070 } };
    uint32_t i = 0;
    for (int k = 0; k < 3; k++) {
        batch[i++] = (2u << 29) | (0x50u << 22) | (3u << 20) | (7 - 2);   /* XY_COLOR_BLT, write RGBA, 7 dwords */
        batch[i++] = (3u << 24) | (0xF0u << 16) | pitch;                   /* 32 bpp, PATCOPY, pitch */
        batch[i++] = (r[k].y << 16) | r[k].x;
        batch[i++] = ((r[k].y + r[k].h) << 16) | (r[k].x + r[k].w);
        batch[i++] = (uint32_t)FB_VA;
        batch[i++] = (uint32_t)(FB_VA >> 32);
        batch[i++] = r[k].rgb;
    }
    batch[i++] = 0x10000002;                                          /* MI_STORE_DATA_IMM through the PPGTT */
    batch[i++] = (uint32_t)(VA + 0x1000); batch[i++] = 0; batch[i++] = MAGIC;
    batch[i++] = 0x13000002; batch[i++] = 0; batch[i++] = 0; batch[i++] = 0;   /* MI_FLUSH_DW: all of it out */
    batch[i++] = 0x05000000;                                          /* MI_BATCH_BUFFER_END */
    done[0] = 0xDEADBEEF;

    if (!gpu_wake(1)) { printf("forcewake: no ack\n"); return 1; }
    static const uint32_t pat[8] = { 3, 1, 2, 0, 3, 3, 3, 3 };          /* 0 WB, 1 WC, 2 WT, 3 UC (as i915) */
    for (int k = 0; k < 8; k++) wr(0x4800 + 4 * k, pat[k]);
    uint32_t g = gpu_ggtt_map(phys, n);
    uint32_t g_hwsp = g, g_ctx = g + 0x1000, g_ring = g + 5 * 0x1000;

    uint32_t q = 0;                                   /* the ring: PDP0, TLB invalidate, the batch */
    ring[q++] = 0x11000000 | (2 * 2 - 1);
    ring[q++] = BCS + 0x274; ring[q++] = (uint32_t)(PHYS(6) >> 32);
    ring[q++] = BCS + 0x270; ring[q++] = (uint32_t)PHYS(6);
    ring[q++] = 0x13000002 | (1u << 18) | (1u << 14);
    ring[q++] = (g_hwsp + 0x800) | (1u << 2); ring[q++] = 0; ring[q++] = 0;
    ring[q++] = 0x18800101; ring[q++] = (uint32_t)VA; ring[q++] = (uint32_t)(VA >> 32);
    ring[q++] = 0;                                    /* MI_NOOP: a whole number of qwords */
    gpu_lrc(regs, BCS, g_ring, q * 4, PHYS(6));
    flush(mem, (size_t)n * 4096);

    int ok = gpu_submit(BCS, g_hwsp, g_ctx, 1, done, MAGIC);
    if (ok) printf("THE GPU DREW THREE RECTANGLES ON THE SCREEN (orange, blue, green): look\n");
    printf("batch address %08x%08x\n", rd(BCS + 0x168), rd(BCS + 0x140));
    gpu_ggtt_unmap();
    gpu_wake(0);
    printf("gpublit: done\n");
    return ok ? 0 : 2;
}
