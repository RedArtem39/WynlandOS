/*
 * WynlandOS - gpurcs: gpucmd's MI_STORE_DATA_IMM on the render engine
 * (RCS0) -- the one OpenGL runs on (tools/hw/gpulib.h).
 *
 * Its context image is far bigger than the blitter's (the render state is
 * saved there when the context switches out): 31 pages after the PPHWSP,
 * more than Gen12 needs, so the save stays in our memory.
 */
#include "gpulib.h"

#define MAGIC 0x8E4DE125u
#define CTX_PAGES 32               /* PPHWSP + 31 of state */

int main(void)
{
    gpu_open();
    /* pages: 0 status, 1..32 context, 33 ring, 34 target */
    uint32_t n = 1 + CTX_PAGES + 2;
    uint64_t phys;
    volatile uint32_t *mem = dma((uint64_t)n * 4096, &phys);
    volatile uint32_t *regs = mem + 2 * 1024, *ring = mem + (1 + CTX_PAGES) * 1024, *target = ring + 1024;
    if (!gpu_wake(1)) { printf("forcewake: no ack\n"); return 1; }
    engine_state(RCS, "RCS before");
    uint32_t g = gpu_ggtt_map(phys, n);
    uint32_t g_hwsp = g, g_ctx = g + 0x1000, g_ring = g + (1 + CTX_PAGES) * 0x1000, g_target = g_ring + 0x1000;
    printf("GGTT %#x: %u pages\n", g, n);

    ring[0] = 0x10400002;                             /* MI_STORE_DATA_IMM, GGTT, one dword */
    ring[1] = g_target;
    ring[2] = 0;
    ring[3] = MAGIC;
    target[0] = 0xDEADBEEF;
    gpu_lrc(regs, RCS, g_ring, 16, 0);
    flush(mem, (size_t)n * 4096);

    int ok = gpu_submit(RCS, g_hwsp, g_ctx, 0, target, MAGIC);
    if (ok) printf("THE RENDER ENGINE RAN OUR COMMAND\n");
    flush(mem, 4096);
    printf("status page at 0x40: %08x %08x %08x %08x\n", mem[16], mem[17], mem[18], mem[19]);
    gpu_ggtt_unmap();
    gpu_wake(0);
    printf("gpurcs: done\n");
    return ok ? 0 : 2;
}
