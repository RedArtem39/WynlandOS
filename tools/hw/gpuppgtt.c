/*
 * WynlandOS - gpuppgtt: a batch buffer through a per-process GTT -- how
 * Mesa feeds the GPU (tools/hw/gpulib.h).
 *
 * On the blitter: 4-level page tables (PML4 -> PDP -> PD -> PT) mapping
 * GPU address 0x10000000 to a batch page and 0x10001000 to a target page;
 * the context in 48-bit PPGTT mode with PDP0 = the PML4. The ring only
 * jumps into the batch (MI_BATCH_BUFFER_START, PPGTT); the batch writes
 * the number through the PPGTT (MI_STORE_DATA_IMM without the GGTT bit)
 * and ends.
 */
#include "gpulib.h"

#define MAGIC 0xBA7C4ED1u
#define VA    0x10000000ull        /* the batch; the target one page on */

int main(void)
{
    gpu_open();
    /* pages: 0 status, 1..4 context, 5 ring, 6 PML4, 7 PDP, 8 PD, 9 PT, 10 batch, 11 target */
    uint32_t n = 12;
    uint64_t phys;
    volatile uint32_t *mem = dma((uint64_t)n * 4096, &phys);
#define PAGE(k) (mem + (k) * 1024)
#define PHYS(k) (phys + (uint64_t)(k) * 4096)
    volatile uint32_t *regs = PAGE(2), *ring = PAGE(5), *batch = PAGE(10), *target = PAGE(11);
    volatile uint64_t *pml4 = (volatile uint64_t *)PAGE(6), *pdp = (volatile uint64_t *)PAGE(7);
    volatile uint64_t *pd = (volatile uint64_t *)PAGE(8), *pt = (volatile uint64_t *)PAGE(9);

    /* the page tables: present | writable; leaf entries PAT index 0 (WB) */
    pml4[(VA >> 39) & 511] = PHYS(7) | 3;
    pdp[(VA >> 30) & 511] = PHYS(8) | 3;
    pd[(VA >> 21) & 511] = PHYS(9) | 3;
    pt[(VA >> 12) & 511] = PHYS(10) | 3;
    pt[((VA >> 12) & 511) + 1] = PHYS(11) | 3;

    batch[0] = 0x10000002;                            /* MI_STORE_DATA_IMM through the PPGTT */
    batch[1] = (uint32_t)(VA + 0x1000);
    batch[2] = (uint32_t)((VA + 0x1000) >> 32);
    batch[3] = MAGIC;
    batch[4] = 0x13000002;                            /* MI_FLUSH_DW: the write out to memory */
    batch[5] = 0;
    batch[6] = 0;
    batch[7] = 0;
    batch[8] = 0x05000000;                            /* MI_BATCH_BUFFER_END */
    batch[9] = 0;
    target[0] = 0xDEADBEEF;

    if (!gpu_wake(1)) { printf("forcewake: no ack\n"); return 1; }
    engine_state(BCS, "BCS before");

    /* the GPU's cache types by PAT index -- i915 sets them up at start
       (tgl_setup_private_ppat): 0 WB, 1 WC, 2 WT, 3 UC, 4-7 WB. Nobody
       did here: what a PPGTT entry's index 0 meant was anyone's guess. */
    printf("PAT before:");
    for (int k = 0; k < 8; k++) printf(" %08x", rd(0x4800 + 4 * k));
    printf("\n");
    static const uint32_t pat[8] = { 3, 1, 2, 0, 3, 3, 3, 3 };
    for (int k = 0; k < 8; k++) wr(0x4800 + 4 * k, pat[k]);
    printf("PAT now:   ");
    for (int k = 0; k < 8; k++) printf(" %08x", rd(0x4800 + 4 * k));
    printf("\n");
    uint32_t g =gpu_ggtt_map(phys, n);
    uint32_t g_hwsp = g, g_ctx = g + 0x1000, g_ring = g + 5 * 0x1000;
    printf("GGTT %#x: %u pages; PPGTT: PML4 at %#llx, batch at GPU %#llx\n", g, n,
           (unsigned long long)PHYS(6), (unsigned long long)VA);

    /* the ring (privileged): the PML4 into PDP0 directly -- a context
       loaded with restore inhibited may not take it from its image, and
       then the walk starts at physical 0 -- the GPU's TLB invalidated
       (a post-sync write is required with it: into the status page),
       then into the batch */
    uint32_t r = 0;
    ring[r++] = 0x11000000 | (2 * 2 - 1);             /* MI_LOAD_REGISTER_IMM(2) */
    ring[r++] = BCS + 0x274; ring[r++] = (uint32_t)(PHYS(6) >> 32);
    ring[r++] = BCS + 0x270; ring[r++] = (uint32_t)PHYS(6);
    ring[r++] = 0x13000002 | (1u << 18) | (1u << 14); /* MI_FLUSH_DW: invalidate TLB, store dword */
    ring[r++] = (g_hwsp + 0x800) | (1u << 2);         /* ... into the status page, GGTT */
    ring[r++] = 0;
    ring[r++] = 0;
    ring[r++] = 0x18800101;                           /* MI_BATCH_BUFFER_START, PPGTT */
    ring[r++] = (uint32_t)VA;
    ring[r++] = (uint32_t)(VA >> 32);
    gpu_lrc(regs, BCS, g_ring, r * 4, PHYS(6));
    flush(mem, (size_t)n * 4096);

    int ok = gpu_submit(BCS, g_hwsp, g_ctx, 1, target, MAGIC);
    printf("PDP0 now %08x%08x (want %llx)  batch address %08x%08x  bb state %08x\n",
           rd(BCS + 0x274), rd(BCS + 0x270), (unsigned long long)PHYS(6), rd(BCS + 0x168), rd(BCS + 0x140), rd(BCS + 0x110));
    flush(regs, 4096);
    printf("saved in the context: PDP0 %08x%08x  ring head %08x tail %08x\n", regs[2 + 26 + 5 + 1 + 15], regs[2 + 26 + 5 + 1 + 17],
           regs[5], regs[7]);
    if (ok) printf("A BATCH RAN THROUGH OUR PAGE TABLES\n");
    flush(mem, 4096);
    printf("status page at 0x40: %08x %08x %08x %08x\n", mem[16], mem[17], mem[18], mem[19]);
    gpu_ggtt_unmap();
    gpu_wake(0);
    printf("gpuppgtt: done\n");
    return ok ? 0 : 2;
}
