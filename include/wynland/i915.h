/*
 * WynlandOS - the Intel GPU driver's inside, shared by its two halves:
 * drivers/gpu/i915.c (the hardware: memory, engines, contexts, requests)
 * and drivers/gpu/i915_uapi.c (Linux's i915 interface, for Mesa).
 */
#pragma once

#include <wynland/types.h>

#define I915_RCS 0x2000u
#define I915_BCS 0x22000u

/* pages, contiguous, and their place in the GGTT */
typedef struct {
    uint64_t phys;
    volatile uint32_t *v;
    uint32_t ggtt;
    uint32_t pages;
} GpuMem;

typedef struct {
    uint32_t base;
    const char *name;
    GpuMem hwsp;                                      /* seqno at dword 0x100 */
    uint32_t seqno;
    bool up;
} Engine;

/* a per-process GPU address space (PPGTT, 4 levels) */
typedef struct {
    GpuMem pml4;
} GpuVm;

typedef struct {
    Engine *e;
    GpuMem img;                                       /* page 0 PPHWSP, page 1.. register state */
    GpuMem ring;                                      /* 4 KB */
    uint32_t tail;
    bool ran;                                         /* submitted before: restore its state */
    uint64_t pml4;                                    /* 0: GGTT only */
    bool inval;                                       /* invalidate the TLB before a request (blitter) */
    bool stamp;                                       /* the engine's timestamp before and after */
} GpuCtx;

extern Engine i915_rcs, i915_bcs;

bool     i915_ready(void);
uint32_t i915_device_id(void);
uint32_t i915_revision(void);
uint32_t i915_rd(uint32_t reg);
bool     i915_fw_get(void);
void     i915_fw_put(void);
void     i915_flush(volatile void *p, uint64_t n);
bool     i915_mem(GpuMem *m, uint32_t pages);          /* + a GGTT place */
bool     i915_vm_create(GpuVm *vm);
bool     i915_vm_map(GpuVm *vm, uint64_t va, uint64_t phys, uint32_t pages, uint32_t pat);
void     i915_vm_unmap(GpuVm *vm, uint64_t va, uint32_t pages);
bool     i915_ctx_create(GpuCtx *c, Engine *e, uint64_t pml4);
uint32_t i915_ctx_submit(GpuCtx *c, const uint32_t *cmds, uint32_t n);
bool     i915_wait_seqno(Engine *e, uint32_t seq, uint32_t ms);
void     i915_engine_reset(Engine *e);
uint32_t i915_cs_timestamp_hz(void);

/* the uAPI (i915_uapi.c): DRM render node clients carry this bit */
#define I915_CLIENT_BIT 0x80000000u
uint32_t i915_open_client(void);
void     i915_client_ref(uint32_t slot);
void     i915_release(uint32_t slot);
int64_t  i915_ioctl(uint32_t slot, uint64_t request, uint64_t argp);
int      i915_mmap_lookup(uint32_t slot, uint64_t offset, uint64_t len, uint64_t *phys);
