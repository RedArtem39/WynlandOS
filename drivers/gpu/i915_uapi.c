/*
 * WynlandOS - Linux's i915 interface on the render node, for Mesa (iris).
 *
 * The render node (/dev/dri/renderD128) is this driver's when the Intel GPU
 * is up (drivers/gpu/i915.c); card0 stays simpledrm (the firmware's
 * framebuffer scans out). What iris uses, in its terms:
 *
 *   objects        GEM_CREATE(_EXT): physically contiguous pages (for now),
 *                  mapped write-back (LLC: coherent with the GPU) through
 *                  MMAP_OFFSET; CLOSE, MADVISE, caching, tiling kept as said
 *   address space  a VM per client (CONTEXT_GETPARAM VM of context 0, or
 *                  VM_CREATE); objects at the addresses Mesa picks
 *                  (EXEC_OBJECT_PINNED, softpin), mapped at execbuffer
 *   contexts       an engine map (CONTEXT_PARAM_ENGINES) of RCS and BCS;
 *                  a hardware context per engine, its state restored
 *   execbuffer     the batch through the PPGTT, caches invalidated before,
 *                  flushed after; SYNCHRONOUS for now: done when it returns,
 *                  so every wait, busy query and syncobj is already signaled
 *   facts          GETPARAM, QUERY (topology from the fuses, engines,
 *                  memory), REG_READ of the timestamp
 *
 * Anything else is logged with its number ("i915: unknown ioctl"): Mesa
 * shows what it still wants.
 */
#include <wynland/types.h>
#include <wynland/i915.h>
#include <wynland/pmm.h>
#include <wynland/memlayout.h>
#include <wynland/spinlock.h>
#include <wynland/waitqueue.h>
#include <wynland/heap.h>
#include <wynland/usercopy.h>

extern void serial_write_string(const char *s);
extern void uint_to_hex(uint64_t v, char *buf);
extern void *memset(void *d, int c, size_t n);
extern void *memcpy(void *d, const void *s, size_t n);
extern void *kmalloc(size_t n);
extern void kfree(void *p);

#define EFAULT 14
#define EINVAL 22
#define ENOMEM 12
#define ENOENT 2
#define ENODEV 19
#define ENOTTY 25
#define ETIME  62

#define MAX_CLIENTS 16
#define MAX_BOS     4096
#define MAX_HANDLES 2048
#define MAX_CTXS    16
#define MAX_VMS     8
#define MAX_SYNCOBJ 256

static void say(const char *s, uint64_t v)
{
    char b[20];
    serial_write_string("i915: ");
    serial_write_string(s);
    uint_to_hex(v, b);
    serial_write_string(b);
    serial_write_string("\r\n");
}

/* ---- objects ------------------------------------------------------------ */

typedef struct {
    bool     used;
    uint32_t refs;                                    /* handles naming it */
    uint64_t phys;
    uint64_t size;
    uint32_t tiling, stride, caching;
    uint64_t last[2];                                 /* the last request using it, per engine (g_q) */
} Bo;

typedef struct {
    bool  used;
    GpuVm vm;
} Vm;

typedef struct {
    bool     used;
    uint32_t vm;                                      /* index into the client's VMs */
    uint32_t nengines;
    uint16_t engine_class[8];                         /* 0 render, 1 copy */
    /* a hardware context per slot of the engine map -- iris maps render
       twice (3D, and compute batches with their own pipeline state), and
       one context shared by both left 3D batches running on the compute
       pipeline: they hung on 3D commands. Without a map: 0 render, 1 copy. */
    GpuCtx   hw[8];
    bool     hw_made[8];
    uint32_t hangs;                                   /* batches of it that hung the GPU */
    bool     banned;                                  /* after a hang: refused (-EIO), as Linux
                                                         does to unrecoverable contexts -- Mesa
                                                         then makes a new one and sets it up */
} Ctx;

typedef struct {
    bool     used;
    uint32_t refs;
    uint32_t bo[MAX_HANDLES];                         /* handle - 1 -> object index + 1 */
    Vm       vms[MAX_VMS];
    Ctx      ctxs[MAX_CTXS];                          /* context 0: the default one */
    struct {
        bool used, signaled;
        uint64_t point;                               /* timeline: reached */
        uint32_t fcls; uint64_t fnum;                 /* binary: its fence -- done when that request is (0: none) */
        uint32_t pcls; uint64_t pnum, ppoint;         /* timeline: the last queued request to signal a point, and it */
    } sync[MAX_SYNCOBJ];
    /* where each object is mapped in this client's VMs (one place each) */
    uint64_t map_va[MAX_HANDLES];
    uint32_t map_vm[MAX_HANDLES];                     /* VM index + 1; 0: not mapped */
    uint32_t busy;                                    /* its requests queued, not done yet */
    bool     dying;                                   /* the last fd closed meanwhile: freed by the last of them */
} Client;

static Bo g_bos[MAX_BOS];
static uint32_t g_resets;                             /* GPU resets since boot (GET_RESET_STATS) */
static Client *g_clients[MAX_CLIENTS];
static Spinlock g_lock = SPINLOCK_INIT("i915-uapi");

static Client *client(uint32_t slot)
{
    slot &= ~I915_CLIENT_BIT;
    if (slot == 0 || slot > MAX_CLIENTS) return NULL;
    Client *c = g_clients[slot - 1];
    return c && c->used ? c : NULL;
}

static Bo *bo_of(Client *c, uint32_t handle)
{
    if (handle == 0 || handle > MAX_HANDLES || !c->bo[handle - 1]) return NULL;
    return &g_bos[c->bo[handle - 1] - 1];
}

static int64_t bo_new(Client *c, uint64_t size, uint32_t *handle)
{
    if (!size || size > (512ull << 20)) return -EINVAL;
    size = (size + 4095) & ~4095ull;
    uint32_t h = 0;
    for (uint32_t i = 0; i < MAX_HANDLES; i++) if (!c->bo[i]) { h = i + 1; break; }
    if (!h) return -ENOMEM;
    uint64_t fl = spin_lock_irqsave(&g_lock);
    uint32_t b = 0;
    for (uint32_t i = 0; i < MAX_BOS; i++) if (!g_bos[i].used) { g_bos[i].used = true; b = i + 1; break; }
    spin_unlock_irqrestore(&g_lock, fl);
    if (!b) return -ENOMEM;
    void *p = pmm_alloc_contiguous((uint32_t)(size / 4096));
    if (!p) {
        g_bos[b - 1].used = false;
        say("no contiguous memory for an object of bytes ", size);
        return -ENOMEM;
    }
    Bo *bo = &g_bos[b - 1];
    bo->phys = (uint64_t)(uintptr_t)p;
    bo->size = size;
    bo->refs = 1;
    bo->tiling = bo->stride = 0;
    bo->last[0] = bo->last[1] = 0;
    bo->caching = 1;
    memset(P2V(bo->phys), 0, size);
    i915_flush(P2V(bo->phys), size);
    c->bo[h - 1] = b;
    c->map_vm[h - 1] = 0;
    *handle = h;
    return 0;
}

static void bo_unref(Bo *bo)
{
    if (--bo->refs) return;
    for (uint64_t o = 0; o < bo->size; o += 4096) pmm_free_page((void *)(uintptr_t)(bo->phys + o));
    bo->used = false;
}

static bool bo_wait(const Bo *bo, uint64_t ms);
static void client_free(Client *c);
static void client_idle(Client *c);

static void handle_close(Client *c, uint32_t h)
{
    Bo *bo = bo_of(c, h);
    if (!bo) return;
    if (c->busy) client_idle(c);                      /* (its own queued work may still use it) */
    if (c->map_vm[h - 1]) {                           /* out of the GPU's view before the pages go */
        Vm *v = &c->vms[c->map_vm[h - 1] - 1];
        if (v->used) i915_vm_unmap(&v->vm, c->map_va[h - 1], (uint32_t)(bo->size / 4096));
        c->map_vm[h - 1] = 0;
    }
    c->bo[h - 1] = 0;
    bo_unref(bo);
}

/* ---- clients ------------------------------------------------------------ */

static bool vm_new(Client *c, uint32_t *id)
{
    for (uint32_t i = 0; i < MAX_VMS; i++) {
        if (c->vms[i].used) continue;
        if (!i915_vm_create(&c->vms[i].vm)) return false;
        c->vms[i].used = true;
        *id = i + 1;
        return true;
    }
    return false;
}

uint32_t i915_open_client(void)
{
    if (!i915_ready()) return 0;
    Client *c = (Client *)kmalloc(sizeof(Client));
    if (!c) return 0;
    memset(c, 0, sizeof *c);
    uint32_t vm = 0;
    if (!vm_new(c, &vm)) { kfree(c); return 0; }
    c->used = true;
    c->refs = 1;
    c->ctxs[0].used = true;                           /* the default context: render, the client's VM */
    c->ctxs[0].vm = vm;
    c->ctxs[0].nengines = 0;
    uint64_t fl = spin_lock_irqsave(&g_lock);
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i]) {
            g_clients[i] = c;
            spin_unlock_irqrestore(&g_lock, fl);
            return (i + 1) | I915_CLIENT_BIT;
        }
    }
    spin_unlock_irqrestore(&g_lock, fl);
    kfree(c);
    return 0;
}

void i915_client_ref(uint32_t slot)
{
    Client *c = client(slot);
    if (c) c->refs++;
}

/* the last fd: its objects closed. (Its VMs' page tables and contexts are
   not given back yet: a few pages a client.) */
void i915_release(uint32_t slot)
{
    Client *c = client(slot);
    if (!c || --c->refs) return;
    g_clients[(slot & ~I915_CLIENT_BIT) - 1] = NULL;
    c->used = false;
    if (c->busy) { c->dying = true; return; }         /* requests still queued: the last one frees it */
    client_free(c);
}

/* its objects closed (once the GPU is done with them), the client gone */
static void client_free(Client *c)
{
    for (uint32_t h = 1; h <= MAX_HANDLES; h++) handle_close(c, h);
    kfree(c);
}

int i915_mmap_lookup(uint32_t slot, uint64_t offset, uint64_t len, uint64_t *phys)
{
    Client *c = client(slot);
    if (!c) return -EINVAL;
    Bo *bo = bo_of(c, (uint32_t)(offset >> 32));
    if (!bo || (offset & 0xFFFFFFFFull) || len > bo->size) return -EINVAL;
    *phys = bo->phys;
    return 0;
}

/* ---- facts: GETPARAM and QUERY ------------------------------------------ */

static uint32_t g_ss_mask, g_eu_mask;                 /* dual subslices; EUs a DSS (bits) */

static void sseu(void)
{
    if (g_ss_mask) return;
    if (!i915_fw_get()) return;
    g_ss_mask = i915_rd(0x913C) & 0x3F;               /* GEN12_GT_DSS_ENABLE */
    uint32_t dis = i915_rd(0x9134) & 0xFF;            /* GEN11_EU_DISABLE: a bit = a pair of EUs */
    i915_fw_put();
    g_eu_mask = 0;
    for (int i = 0; i < 8; i++) if (!(dis & (1u << i))) g_eu_mask |= 3u << (2 * i);
    if (!g_ss_mask) g_ss_mask = 0x3F;
}

static int popcount(uint32_t v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

static int64_t getparam(uint64_t argp)
{
    struct { int32_t param; uint32_t pad; uint64_t value; } gp;   /* the value is a user pointer */
    if (copy_from_user(&gp, (void *)argp, sizeof gp)) return -EFAULT;
    int32_t v;
    sseu();
    switch (gp.param) {
    case 4:  v = (int32_t)i915_device_id(); break;    /* CHIPSET_ID */
    case 5:  v = 1; break;                            /* HAS_GEM */
    /* the old yes/no abilities, as i915 answers them on Gen12 (anv checks
       a set of them: "kernel missing gem wait" was HAS_WAIT_TIMEOUT) */
    case 9:  case 11: case 12: case 13: case 14: case 15: case 16:
    case 19: case 21: case 24: case 35:
        v = 1; break;                                 /* EXECBUF2, BLT, RELAXED_FENCING, COHERENT_RINGS,
                                                         EXEC_CONSTANTS, RELAXED_DELTA, GEN7_SOL_RESET,
                                                         WAIT_TIMEOUT, PRIME_VMAP_FLUSH, PINNED_BATCHES, GPU_RESET */
    case 10: case 20: case 22: case 23: case 28: case 31: case 36:
        v = 0; break;                                 /* BSD, SEMAPHORES, VEBOX, SECURE_BATCHES, CMD_PARSER_VERSION,
                                                         POOLED_EU, RESOURCE_STREAMER */
    case 17: v = 1; break;                            /* HAS_LLC */
    case 18: v = 3; break;                            /* HAS_ALIASING_PPGTT: full 48-bit */
    case 25: v = 1; break;                            /* HAS_EXEC_NO_RELOC */
    case 26: v = 1; break;                            /* HAS_EXEC_HANDLE_LUT */
    case 27: v = 0; break;                            /* HAS_WT */
    case 30: v = 1; break;                            /* MMAP_VERSION */
    case 32: v = (int32_t)i915_revision(); break;     /* REVISION */
    case 33: v = popcount(g_ss_mask); break;          /* SUBSLICE_TOTAL */
    case 34: v = popcount(g_ss_mask) * popcount(g_eu_mask); break;   /* EU_TOTAL */
    case 37: v = 1; break;                            /* HAS_EXEC_SOFTPIN */
    case 39: v = 0; break;                            /* MIN_EU_IN_POOL */
    case 40: v = 4; break;                            /* MMAP_GTT_VERSION: MMAP_OFFSET */
    case 41: v = 1; break;                            /* HAS_SCHEDULER: enabled */
    case 42: v = 1; break;                            /* HAS_COHERENT_RINGS */
    case 43: v = 1; break;                            /* HAS_EXEC_ASYNC */
    case 44: v = 0; break;                            /* HAS_EXEC_FENCE (sync files): no */
    case 45: v = 1; break;                            /* HAS_EXEC_CAPTURE */
    case 46: v = 1; break;                            /* SLICE_MASK */
    case 47: v = (int32_t)g_ss_mask; break;           /* SUBSLICE_MASK */
    case 48: v = 1; break;                            /* HAS_EXEC_BATCH_FIRST */
    case 49: v = 1; break;                            /* HAS_EXEC_FENCE_ARRAY */
    case 50: v = 3; break;                            /* HAS_CONTEXT_ISOLATION: render, copy */
    case 51: v = (int32_t)i915_cs_timestamp_hz(); break;   /* CS_TIMESTAMP_FREQUENCY */
    case 53: v = 0; break;                            /* HAS_EXEC_SUBMIT_FENCE */
    case 55: v = 1; break;                            /* HAS_EXEC_TIMELINE_FENCES */
    case 56: v = 0; break;                            /* HAS_USERPTR_PROBE */
    case 1: case 54: case 58:                         /* IRQ_ACTIVE, PERF_REVISION, PXP_STATUS: no */
        return -ENODEV;
    default:
        say("GETPARAM not known: ", (uint64_t)(int64_t)gp.param);
        return -EINVAL;
    }
    return copy_to_user((void *)gp.value, &v, sizeof v) ? -EFAULT : 0;
}

/* one QUERY item: its length asked (data 0) or its data written */
static int32_t query_item(uint64_t id, uint64_t data, int32_t len)
{
    static uint8_t buf[512];
    int32_t n = 0;
    memset(buf, 0, sizeof buf);
    sseu();
    if (id == 1) {                                    /* TOPOLOGY_INFO: 1 slice, 6 DSS, 16 EUs each */
        uint16_t *h = (uint16_t *)buf;
        h[0] = 0; h[1] = 1; h[2] = 6; h[3] = 16;      /* flags, max slices, subslices, EUs */
        h[4] = 1; h[5] = 1;                           /* subslice offset, stride */
        h[6] = 2; h[7] = 2;                           /* EU offset, stride */
        uint8_t *d = buf + 16;
        d[0] = 1;                                     /* slice mask */
        d[1] = (uint8_t)g_ss_mask;                    /* subslices of slice 0 */
        for (int s = 0; s < 6; s++) {
            uint32_t m = (g_ss_mask & (1u << s)) ? g_eu_mask : 0;
            d[2 + 2 * s] = (uint8_t)m;
            d[3 + 2 * s] = (uint8_t)(m >> 8);
        }
        n = 16 + 2 + 12;
    } else if (id == 2) {                             /* ENGINE_INFO: render 0:0, copy 1:0 */
        uint32_t *w = (uint32_t *)buf;
        w[0] = 2;                                     /* num_engines; rsvd[3] */
        uint8_t *e = buf + 16;                        /* struct drm_i915_engine_info: 56 bytes */
        for (int k = 0; k < 2; k++, e += 56) {
            uint16_t *cls = (uint16_t *)e;
            cls[0] = (uint16_t)k; cls[1] = 0;         /* class, instance */
            *(uint64_t *)(e + 8) = 1;                 /* flags: HAS_LOGICAL_INSTANCE */
            *(uint16_t *)(e + 24) = 0;                /* logical instance */
        }
        n = 16 + 2 * 56;
    } else if (id == 4) {                             /* MEMORY_REGIONS: system memory */
        extern uint64_t pmm_get_total_memory(void);
        extern uint64_t pmm_get_free_memory(void);
        uint32_t *w = (uint32_t *)buf;
        w[0] = 1;
        uint8_t *r = buf + 16;                        /* struct drm_i915_memory_region_info: 88 bytes */
        *(uint16_t *)r = 0; *(uint16_t *)(r + 2) = 0; /* class SYSTEM, instance 0 */
        *(uint64_t *)(r + 8) = pmm_get_total_memory();
        *(uint64_t *)(r + 16) = pmm_get_free_memory();
        *(uint64_t *)(r + 24) = pmm_get_total_memory();
        *(uint64_t *)(r + 32) = pmm_get_free_memory();
        n = 16 + 88;
    } else {
        say("QUERY not known (an error to the caller): ", id);
        return -EINVAL;
    }
    if (!len) return n;                               /* the size asked first */
    if (len < n) return -EINVAL;
    return copy_to_user((void *)data, buf, (uint64_t)n) ? -EFAULT : n;
}

static int64_t query(uint64_t argp)
{
    struct { uint32_t num_items, flags; uint64_t items_ptr; } q;
    if (copy_from_user(&q, (void *)argp, sizeof q)) return -EFAULT;
    for (uint32_t i = 0; i < q.num_items; i++) {
        struct { uint64_t query_id; int32_t length; uint32_t flags; uint64_t data_ptr; } it;
        uint64_t at = q.items_ptr + (uint64_t)i * sizeof it;
        if (copy_from_user(&it, (void *)at, sizeof it)) return -EFAULT;
        it.length = query_item(it.query_id, it.data_ptr, it.length);
        if (copy_to_user((void *)at, &it, sizeof it)) return -EFAULT;
    }
    return 0;
}

/* ---- contexts ------------------------------------------------------------ */

static int64_t ctx_setparam(Client *c, Ctx *x, uint64_t param, uint32_t size, uint64_t value)
{
    switch (param) {
    case 0x9:                                         /* VM */
        if (value == 0 || value > MAX_VMS || !c->vms[value - 1].used) return -ENOENT;
        x->vm = (uint32_t)value;
        memset(x->hw_made, 0, sizeof x->hw_made);   /* (hardware contexts hold their VM: new ones) */
        return 0;
    case 0xa: {                                       /* ENGINES: { u64 extensions; {u16 class, u16 instance}[] } */
        if (size < 8) { x->nengines = 0; return 0; }
        uint32_t n = (size - 8) / 4;
        if (n > 8) return -EINVAL;
        for (uint32_t k = 0; k < n; k++) {
            uint16_t ci[2];
            if (copy_from_user(ci, (void *)(value + 8 + 4 * k), 4)) return -EFAULT;
            if (ci[0] > 1 || ci[1] != 0) { say("ENGINES: no engine of class ", ci[0]); return -EINVAL; }
            x->engine_class[k] = ci[0];
        }
        x->nengines = n;
        memset(x->hw_made, 0, sizeof x->hw_made);   /* (slots of the old map) */
        return 0;
    }
    case 0x4: case 0x5: case 0x6: case 0x8: case 0xb: case 0xc: case 0xe:
        return 0;                                     /* error capture, bannable, priority, recoverable, persistence, ring size, low latency */
    default:
        say("CONTEXT_SETPARAM not known: ", param);
        return -EINVAL;
    }
}

static int64_t ctx_create(Client *c, uint64_t argp)
{
    struct { uint32_t ctx_id, flags; uint64_t extensions; } a;
    if (copy_from_user(&a, (void *)argp, sizeof a)) return -EFAULT;
    uint32_t id = 0;
    for (uint32_t i = 1; i < MAX_CTXS; i++) if (!c->ctxs[i].used) { id = i; break; }
    if (!id) return -ENOMEM;
    Ctx *x = &c->ctxs[id];
    memset(x, 0, sizeof *x);
    x->used = true;
    x->vm = c->ctxs[0].vm;
    /* extensions: SETPARAM (name 0) { next, name, flags, rsvd[4]; param } */
    uint64_t e = (a.flags & 1) ? a.extensions : 0;
    for (int guard = 0; e && guard < 16; guard++) {
        struct { uint64_t next; uint32_t name, flags, rsvd[4]; uint32_t ctx_id, size; uint64_t param, value; } ext;
        if (copy_from_user(&ext, (void *)e, sizeof ext)) { x->used = false; return -EFAULT; }
        if (ext.name != 0) { say("CONTEXT_CREATE extension not known: ", ext.name); x->used = false; return -EINVAL; }
        int64_t r = ctx_setparam(c, x, ext.param, ext.size, ext.value);
        if (r) { x->used = false; return r; }
        e = ext.next;
    }
    a.ctx_id = id;
    return copy_to_user((void *)argp, &a, sizeof a) ? -EFAULT : 0;
}

static int64_t ctx_param(Client *c, uint64_t argp, bool set)
{
    struct { uint32_t ctx_id, size; uint64_t param, value; } p;
    if (copy_from_user(&p, (void *)argp, sizeof p)) return -EFAULT;
    if (p.ctx_id >= MAX_CTXS || !c->ctxs[p.ctx_id].used) return -ENOENT;
    Ctx *x = &c->ctxs[p.ctx_id];
    if (set) return ctx_setparam(c, x, p.param, p.size, p.value);
    switch (p.param) {
    case 0x3: p.value = 1ull << 48; break;            /* GTT_SIZE */
    case 0x9: p.value = x->vm; break;                 /* VM */
    case 0x6: p.value = 0; break;                     /* PRIORITY */
    case 0x5: case 0x8: case 0xb: p.value = 1; break; /* bannable, recoverable, persistence */
    case 0x4: p.value = 0; break;
    default:
        say("CONTEXT_GETPARAM not known: ", p.param);
        return -EINVAL;
    }
    return copy_to_user((void *)argp, &p, sizeof p) ? -EFAULT : 0;
}

/* ---- execbuffer ---------------------------------------------------------- */

#define EXEC_MAX 4096

static void sync_signal(Client *c, uint32_t h, uint64_t point);
static bool sync_done(Client *c, uint32_t h, uint64_t point, bool *bad);
static void client_free(Client *c);

/* A queue per engine. execbuffer checks a request, maps its objects and
   queues it, and returns: Mesa builds the next frame while the GPU draws
   this one. A kernel thread per engine (eng_thread) puts the requests into
   the hardware one at a time, in order, and when one is done signals its
   fences and lets go of its objects.
   One at a time: a second context put into the engine while another ran
   replaced that one in the middle of its batch (never put back: it looked
   like a hang). In order: that is also what keeps one process's drawing
   ahead of another's reading it (Zerp sampling a client's buffer). */
typedef struct Req {
    struct Req *next;
    Client  *c;
    Ctx     *x;                                       /* (CONTEXT_DESTROY waits for its requests) */
    uint32_t slot, cls;
    uint64_t num;                                     /* its place in the engine's order */
    uint64_t batch_va;
    uint32_t n, cmds[48];
    uint32_t nheld, *held;                            /* objects it holds (indices + 1) */
    uint32_t nin, nout, *fh;                          /* fences: waited for [0, nin), signaled [nin, nin + nout) */
    uint64_t *fp;                                     /* their timeline points (0: binary) */
} Req;

typedef struct {
    Req *head, *tail;
    uint64_t queued, done;                            /* request numbers: the last queued, the last done */
    WaitQueue wq;                                     /* the engine's thread, for new requests */
    WaitQueue done_wq;                                /* waiters for `done` */
    bool thread;
} Queue;

static Queue g_q[2];                                  /* render, copy */
/* where each engine's thread is (said by waits that last) */
static const char *volatile g_eng_at[2] = { "not started", "not started" };
static volatile uint64_t g_eng_req[2];

static bool bo_idle(const Bo *bo)
{
    return bo->last[0] <= g_q[0].done && bo->last[1] <= g_q[1].done;
}

/* until the GPU is done with the object: false if not within `ms` */
static bool bo_wait(const Bo *bo, uint64_t ms)
{
    extern uint64_t timer_get_ms(void);
    uint64_t until = timer_get_ms() + ms;
    while (!bo_idle(bo)) {
        if (timer_get_ms() >= until) {
            say("an object never got idle: last render request ", bo->last[0]);
            say("  render done ", g_q[0].done);
            say("  last copy request ", bo->last[1]);
            say("  copy done ", g_q[1].done);
            return false;
        }
        uint64_t seq = waitqueue_seq();
        if (bo_idle(bo)) break;
        uint32_t e = bo->last[0] > g_q[0].done ? 0 : 1;
        waitqueue_wait_ms_seq(&g_q[e].done_wq, timer_get_ms() + 5, seq);
    }
    return true;
}

/* for the display (drivers/video/virtgpu_drm.c): a frame drawn before it is shown */
bool i915_bo_wait_ref(uint32_t ref, uint32_t ms)
{
    if (!ref || ref > MAX_BOS || !g_bos[ref - 1].used) return true;
    return bo_wait(&g_bos[ref - 1], ms);
}

/* every queued request of the client done (before its contexts or itself go) */
static void client_idle(Client *c)
{
    extern void sched_sleep_ms(uint64_t ms);
    uint32_t n = 0;
    while (c->busy) {
        sched_sleep_ms(1);
        if (++n % 1000 == 0) {
            say("waiting for a client's queued requests: still ", c->busy);
            for (int e = 0; e < 2; e++) {
                say(e ? "  copy queued/done " : "  render queued/done ", (g_q[e].queued << 32) | g_q[e].done);
                serial_write_string(e ? "i915:   copy thread: " : "i915:   render thread: ");
                serial_write_string(g_eng_at[e]);
                say(", request ", g_eng_req[e]);
                say("    queue head ", g_q[e].head ? g_q[e].head->num : 0);
            }
        }
    }
}

static void req_free(Req *r)
{
    for (uint32_t i = 0; i < r->nheld; i++) bo_unref(&g_bos[r->held[i] - 1]);
    kfree(r->held);
    if (r->fh) kfree(r->fh);
    if (r->fp) kfree(r->fp);
    kfree(r);
}

/* one request through the hardware: its fences in, the batch, its fences out */
static void eng_run(Req *r)
{
    extern uint64_t timer_get_ms(void);
    extern void sched_sleep_ms(uint64_t ms);
    extern uint64_t sched_clock_ns(void);
    Client *c = r->c;
    Ctx *x = r->x;
    Engine *e = r->cls ? &i915_bcs : &i915_rcs;
    g_eng_req[r->cls] = r->num;
    g_eng_at[r->cls] = "waiting for another engine's request";
    /* what it waits for (another engine's work, a CPU signal): 10 s at most */
    uint64_t waited0 = timer_get_ms();
    for (uint32_t i = 0; i < r->nin; i++) {
        uint64_t until = timer_get_ms() + 10000;
        bool bad = false;
        if (!(r->fh[i] & 0x80000000u)) continue;
        uint32_t oc = r->fh[i] & 1;
        if (oc == r->cls) continue;                   /* the same queue: ahead of it already */
        Queue *o = &g_q[oc];
        while (o->done < r->fp[i] && timer_get_ms() < until) sched_sleep_ms(1);
        (void)bad;
    }
    if (timer_get_ms() - waited0 > 200) {
        say("a request waited for its fences, ms ", timer_get_ms() - waited0);
        for (uint32_t i = 0; i < r->nin && i < 4; i++) { say("  fence ", r->fh[i]); say("    point/number ", r->fp[i]); }
    }
    g_eng_at[r->cls] = "forcewake";
    if (!x->banned && !c->dying && i915_fw_get()) {
        g_eng_at[r->cls] = "the GPU on it";
        uint64_t w0 = sched_clock_ns();
        uint32_t seq = i915_ctx_submit(&x->hw[r->slot], r->cmds, r->n);
        bool done = i915_wait_seqno(e, seq, 3000);
        {
            /* how long the GPU takes, a line every 5 s */
            static uint64_t n_eb, busy_ns, since;
            uint64_t w1 = sched_clock_ns();
            n_eb++;
            if (w1 > w0 && w1 - w0 < 10000000000ull) busy_ns += w1 - w0;   /* (the clock of another core may run behind) */
            if (!since) since = w1;
            if (w1 - since > 5000000000ull) {
                extern uint64_t i915_irq_count(void);
                say("batches in 5 s ", n_eb);
                say("  the GPU on them, ms ", busy_ns / 1000000);
                say("  GPU interrupts so far ", i915_irq_count());
                n_eb = busy_ns = 0;
                since = w1;
            }
        }
        if (!done) {
            say("execbuffer: the GPU did not finish in 3 s, engine ", e->base);
            say("  ACTHD ", i915_rd(e->base + 0x74));
            say("  IPEHR ", i915_rd(e->base + 0x68));
            say("  ACTHD_UDW ", i915_rd(e->base + 0x5C));
            say("  batch at (low) ", (uint32_t)r->batch_va);
            /* a GPU page fault (Gen12): valid bit 0 of the fault register,
               the address from the TLB data (bits 12..47) */
            uint32_t fault = i915_rd(0xCEC4);
            say("  fault reg ", fault);
            if (fault & 1) {
                say("  fault addr >> 12 (low) ", i915_rd(0xCEB8));
                say("  fault addr bits 44.. ", i915_rd(0xCEBC) & 0xF);
            }
            say("  EIR ", i915_rd(e->base + 0xB0));
            say("  ESR ", i915_rd(e->base + 0xB8));
            i915_engine_reset(e);                     /* (forcewake still held) */
            g_resets++;
            x->hangs++;
            x->banned = true;                         /* later batches -EIO: Mesa replaces the context */
        }
        i915_fw_put();
    }
    g_eng_at[r->cls] = "signaling";
    /* its fences signaled even when it failed: no waiter stuck */
    for (uint32_t i = 0; i < r->nout; i++) sync_signal(c, r->fh[r->nin + i], r->fp[r->nin + i]);
}

static void eng_thread(void *arg)
{
    extern uint64_t timer_get_ms(void);
    Queue *q = &g_q[(uintptr_t)arg];
    for (;;) {
        uint64_t seq = waitqueue_seq();
        Req *r = q->head;
        uint32_t me = (uint32_t)(q - g_q);
        if (!r) { g_eng_at[me] = "idle"; waitqueue_wait_ms_seq(&q->wq, timer_get_ms() + 1000, seq); continue; }
        eng_run(r);
        q->head = r->next;
        if (!q->head) q->tail = NULL;
        q->done = r->num;
        Client *c = r->c;
        g_eng_at[me] = "freeing a request";
        req_free(r);
        g_eng_at[me] = "freeing a client";
        if (!--c->busy && c->dying) client_free(c);   /* its last fd went while this ran */
        g_eng_at[me] = "after a request";
        waitqueue_wake_all(&q->done_wq);
    }
}

/* The engines' threads, made once by the driver's own kernel thread
   (drivers/gpu/i915.c): thread_create() puts a thread into the caller's
   process, and made by the first execbuffer they were the login screen's
   -- killed with it at login, and nothing ran the queues any more. */
void i915_uapi_start(void)
{
    extern struct Thread *thread_create(void (*entry)(void *), void *arg);
    for (uintptr_t e = 0; e < 2; e++)
        if (!g_q[e].thread) g_q[e].thread = thread_create(eng_thread, (void *)e) != NULL;
}

/* the fences of an execbuffer: FENCE_ARRAY's { handle, flags } pairs
   (flags: 1 wait, 2 signal) or the TIMELINE_FENCES extension's handles
   and points. Into r->fh/fp: the waited-for ones first. */
static int64_t eb_fences(Client *c, Req *r, uint64_t flags, uint64_t ptr, uint32_t count)
{
    struct { uint32_t h, f; uint64_t p; } tmp[64];
    uint32_t n = 0;
    if ((flags & (1u << 19)) && count && ptr) {       /* FENCE_ARRAY */
        for (uint32_t i = 0; i < count; i++) {
            uint32_t f[2];
            if (copy_from_user(f, (void *)(ptr + 8ull * i), 8)) return -EFAULT;
            if (n < 64) { tmp[n].h = f[0]; tmp[n].f = f[1]; tmp[n].p = 0; n++; }
        }
    }
    if ((flags & (1u << 21)) && ptr) {                /* USE_EXTENSIONS */
        uint64_t e = ptr;
        for (int guard = 0; e && guard < 8; guard++) {
            struct { uint64_t next; uint32_t name, flags, rsvd[4]; uint64_t count, handles, values; } x;
            if (copy_from_user(&x, (void *)e, sizeof x)) return -EFAULT;
            if (x.name == 0) {                        /* TIMELINE_FENCES */
                for (uint64_t i = 0; i < x.count && n < 64; i++) {
                    uint32_t f[2];
                    uint64_t v = 0;
                    if (copy_from_user(f, (void *)(x.handles + 8 * i), 8)) return -EFAULT;
                    if (x.values && copy_from_user(&v, (void *)(x.values + 8 * i), 8)) return -EFAULT;
                    tmp[n].h = f[0]; tmp[n].f = f[1]; tmp[n].p = v; n++;
                }
            }
            e = x.next;
        }
    }
    if (!n) return 0;
    r->fh = kmalloc(n * sizeof *r->fh);
    r->fp = kmalloc(n * sizeof *r->fp);
    if (!r->fh || !r->fp) return -ENOMEM;
    for (uint32_t pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < n; i++) {
            if (!(tmp[i].f & (pass ? 2u : 1u))) continue;
            uint32_t h = tmp[i].h;
            bool known = h && h <= MAX_SYNCOBJ && c->sync[h - 1].used;
            if (pass) {                               /* signaled: binary ones by fence (r->num, set at queueing) */
                if (!tmp[i].p) {
                    if (known) { c->sync[h - 1].signaled = false; c->sync[h - 1].fnum = ~0ull; c->sync[h - 1].fcls = 0; }
                    continue;                         /* (the fence: filled in by execbuffer) */
                }
                if (known && tmp[i].p > c->sync[h - 1].ppoint) {   /* this request will reach that point */
                    c->sync[h - 1].ppoint = tmp[i].p;
                    c->sync[h - 1].pnum = ~0ull;      /* (filled in by execbuffer) */
                }
                uint32_t k = r->nin + r->nout++;
                r->fh[k] = h; r->fp[k] = tmp[i].p;
                continue;
            }
            /* waited for */
            if (known && !tmp[i].p && !c->sync[h - 1].signaled && c->sync[h - 1].fnum) {
                uint32_t fc = c->sync[h - 1].fcls;
                uint64_t fn = c->sync[h - 1].fnum;
                if (fn == ~0ull || fn <= g_q[fc].done) continue;   /* done already (or this very request's) */
                uint32_t k = r->nin++;
                r->fh[k] = 0x80000000u | fc;          /* a request of an engine: its number */
                r->fp[k] = fn;
                continue;
            }
            if (!known) continue;
            if (tmp[i].p && c->sync[h - 1].point < tmp[i].p && c->sync[h - 1].ppoint >= tmp[i].p &&
                c->sync[h - 1].pnum && c->sync[h - 1].pnum != ~0ull && c->sync[h - 1].pnum > g_q[c->sync[h - 1].pcls].done) {
                uint32_t k = r->nin++;                /* the queued request that reaches that point */
                r->fh[k] = 0x80000000u | c->sync[h - 1].pcls;
                r->fp[k] = c->sync[h - 1].pnum;
            }
            /* anything else: signaled already, or nothing queued will signal
               it -- Linux refuses those; we go on rather than wait forever */
        }
    return 0;
}

static int64_t execbuffer(Client *c, uint64_t argp)
{
    struct {
        uint64_t buffers_ptr; uint32_t buffer_count, batch_start_offset, batch_len, DR1, DR4, num_cliprects;
        uint64_t cliprects_ptr, flags, rsvd1, rsvd2;
    } eb;
    if (copy_from_user(&eb, (void *)argp, sizeof eb)) return -EFAULT;
    if (!eb.buffer_count || eb.buffer_count > EXEC_MAX) return -EINVAL;
    uint32_t ctx_id = (uint32_t)eb.rsvd1;
    if (ctx_id >= MAX_CTXS || !c->ctxs[ctx_id].used) return -ENOENT;
    Ctx *x = &c->ctxs[ctx_id];
    if (x->banned) return -5;                         /* -EIO: it hung the GPU; Mesa makes a new one */
    Vm *v = &c->vms[x->vm - 1];
    /* the engine: an index into the context's map, or the legacy ring */
    uint32_t sel = (uint32_t)(eb.flags & 0x3F), cls, slot;
    if (x->nengines) {
        if (sel >= x->nengines) return -EINVAL;
        cls = x->engine_class[sel];
        slot = sel;
    } else {
        cls = (sel == 3) ? 1 : 0;                     /* I915_EXEC_BLT, else render */
        slot = cls;
    }
    Req *r = kmalloc(sizeof *r);
    if (!r) return -ENOMEM;
    memset(r, 0, sizeof *r);
    /* the objects it uses stay alive until the GPU is done with them */
    r->held = kmalloc((size_t)eb.buffer_count * sizeof *r->held);
    if (!r->held) { kfree(r); return -ENOMEM; }
    int64_t err = 0;
#define EB_FAIL(e_) do { err = (e_); goto fail; } while (0)

    /* the objects: each at its address in the VM (softpin) */
    struct exo { uint32_t handle, relocation_count; uint64_t relocs_ptr, alignment, offset, flags, rsvd1, rsvd2; };
    uint32_t batch_idx = (eb.flags & (1u << 18)) ? 0 : eb.buffer_count - 1;   /* BATCH_FIRST */
    for (uint32_t i = 0; i < eb.buffer_count; i++) {
        struct exo o;
        if (copy_from_user(&o, (void *)(eb.buffers_ptr + (uint64_t)i * sizeof o), sizeof o)) EB_FAIL(-EFAULT);
        uint32_t h = o.handle;
        if (eb.flags & (1u << 12)) { /* HANDLE_LUT: the handle is as given (no LUT of our own) */ }
        Bo *bo = bo_of(c, h);
        if (!bo) { say("execbuffer: no object, handle ", h); EB_FAIL(-ENOENT); }
        if (o.relocation_count) { say("execbuffer: relocations are not done, handle ", h); EB_FAIL(-EINVAL); }
        r->held[r->nheld++] = c->bo[h - 1];
        bo->refs++;
        if (!(o.flags & (1u << 4))) {                 /* not pinned: its place now, at the next free one */
            static uint64_t next = 0x100000000ull;
            if (!c->map_vm[h - 1]) { o.offset = next; next += bo->size + 4096; }
            else o.offset = c->map_va[h - 1];
            if (copy_to_user((void *)(eb.buffers_ptr + (uint64_t)i * sizeof o + 24), &o.offset, 8)) EB_FAIL(-EFAULT);
        }
        uint64_t va = o.offset & ((1ull << 48) - 1);
        if (c->map_vm[h - 1] != x->vm || c->map_va[h - 1] != va) {
            /* moved: not while queued work still uses the old place */
            if (c->map_vm[h - 1] && !bo_wait(bo, 3000)) EB_FAIL(-5);
            if (c->map_vm[h - 1]) i915_vm_unmap(&c->vms[c->map_vm[h - 1] - 1].vm, c->map_va[h - 1], (uint32_t)(bo->size / 4096));
            c->map_vm[h - 1] = 0;
            if (!i915_vm_map(&v->vm, va, bo->phys, (uint32_t)(bo->size / 4096), 0)) {
                i915_vm_unmap(&v->vm, va, (uint32_t)(bo->size / 4096));   /* (none of it left half-made) */
                EB_FAIL(-ENOMEM);
            }
            c->map_vm[h - 1] = x->vm;
            c->map_va[h - 1] = va;
        }
        if (i == batch_idx) r->batch_va = va + eb.batch_start_offset;
    }

    /* the hardware context for that slot, in the context's VM */
    Engine *e = cls ? &i915_bcs : &i915_rcs;
    if (!x->hw_made[slot]) {
        if (!i915_ctx_create(&x->hw[slot], e, v->vm.pml4.phys)) EB_FAIL(-ENOMEM);
        x->hw[slot].inval = (cls == 1);
        x->hw_made[slot] = true;
    }
    /* around the batch, as Linux's gen12_emit_flush_rcs and emit_bb_start */
    uint32_t *cmds = r->cmds, n = 0;
    if (cls == 0) {
        cmds[n++] = 0x02800101;                       /* MI_ARB_CHECK: the pre-parser off while caches go */
        /* PIPE_CONTROL: caches and TLB invalidated before the batch (a
           post-sync write with it: into the status page) */
        cmds[n++] = 0x7A000004;
        cmds[n++] = (1u << 20) | (1u << 18) | (1u << 11) | (1u << 10) | (1u << 4) | (1u << 3) | (1u << 2) |
                    (1u << 29) | (1u << 14) | (1u << 24);   /* CS stall, TLB, instr, texture, VF, const, state,
                                                               command cache; write imm, GGTT */
        cmds[n++] = e->hwsp.ggtt + 0x900; cmds[n++] = 0; cmds[n++] = 0; cmds[n++] = 0;
        /* the compression (aux) table's cache invalidated too, and waited
           for (the bit clears when done): GFX_CCS_AUX_NV */
        cmds[n++] = 0x11000001; cmds[n++] = 0x4208; cmds[n++] = 1;   /* MI_LOAD_REGISTER_IMM */
        cmds[n++] = 0x0E01C003;                       /* MI_SEMAPHORE_WAIT: register poll until == 0 */
        cmds[n++] = 0; cmds[n++] = 0x4208; cmds[n++] = 0; cmds[n++] = 0;
        cmds[n++] = 0;                                /* MI_NOOP */
        cmds[n++] = 0x02800100;                       /* MI_ARB_CHECK: the pre-parser on again */
        n += i915_rcs_ctx_workarounds(&cmds[n]);      /* the context's own workarounds */
    }
    cmds[n++] = 0x18800101;                           /* MI_BATCH_BUFFER_START, PPGTT */
    cmds[n++] = (uint32_t)r->batch_va;
    cmds[n++] = (uint32_t)(r->batch_va >> 32);
    if (cls == 0) {
        /* PIPE_CONTROL: render target, depth, data port, tile caches
           flushed (Gen12: the HDC pipeline too, a bit of dword 0) */
        cmds[n++] = 0x7A000004 | (1u << 9);
        cmds[n++] = (1u << 20) | (1u << 12) | (1u << 0) | (1u << 5) | (1u << 28) | (1u << 14) | (1u << 24) |
                    (1u << 27) | (1u << 13) | (1u << 7);   /* + L3 flush, depth stall (Wa_1409600907), flush enable */
        cmds[n++] = e->hwsp.ggtt + 0x900; cmds[n++] = 0; cmds[n++] = 0; cmds[n++] = 0;
    } else {
        cmds[n++] = 0x13000002; cmds[n++] = 0; cmds[n++] = 0; cmds[n++] = 0;   /* MI_FLUSH_DW */
    }
    r->n = n;
    if ((err = eb_fences(c, r, eb.flags, eb.cliprects_ptr, eb.num_cliprects))) goto fail;
#undef EB_FAIL

    /* queued: its objects marked busy until it is done */
    Queue *q = &g_q[cls];
    r->c = c; r->x = x; r->slot = slot; r->cls = cls;
    r->num = ++q->queued;
    for (uint32_t i = 0; i < r->nheld; i++) g_bos[r->held[i] - 1].last[cls] = r->num;
    for (uint32_t i = 0; i < MAX_SYNCOBJ; i++) {
        if (!c->sync[i].used) continue;
        if (c->sync[i].fnum == ~0ull) { c->sync[i].fcls = cls; c->sync[i].fnum = r->num; }
        if (c->sync[i].pnum == ~0ull) { c->sync[i].pcls = cls; c->sync[i].pnum = r->num; }
    }
    c->busy++;
    if (q->tail) q->tail->next = r; else q->head = r;
    q->tail = r;
    if (!q->thread) say("execbuffer: no thread for the engine (i915_uapi_start not run): queued only, class ", cls);
    waitqueue_wake_all(&q->wq);
    return 0;

fail:
    req_free(r);
    return err;
}

/* ---- syncobjs ----------------------------------------------------------- */

/* Binary ones are signaled or not; timeline ones have reached a point.
   Work is done when execbuffer returns (synchronous), so a wait is for
   another thread's submission or a CPU signal, with the absolute
   CLOCK_MONOTONIC timeout DRM gives. */
static void sync_signal(Client *c, uint32_t h, uint64_t point)
{
    if (!h || h > MAX_SYNCOBJ || !c->sync[h - 1].used) return;
    c->sync[h - 1].signaled = true;
    c->sync[h - 1].fnum = 0;
    if (point > c->sync[h - 1].point) c->sync[h - 1].point = point;
}

static bool sync_done(Client *c, uint32_t h, uint64_t point, bool *bad)
{
    if (!h || h > MAX_SYNCOBJ || !c->sync[h - 1].used) { *bad = true; return true; }
    if (point) return c->sync[h - 1].point >= point;
    if (c->sync[h - 1].signaled) return true;
    return c->sync[h - 1].fnum && g_q[c->sync[h - 1].fcls].done >= c->sync[h - 1].fnum;
}

static int64_t sync_wait(Client *c, uint64_t handles, uint64_t points, uint32_t count, uint32_t flags,
                         int64_t timeout_ns, uint64_t argp, uint32_t first_off)
{
    extern uint64_t sched_clock_ns(void);
    extern void sched_sleep_ms(uint64_t ms);
    extern uint64_t timer_get_ms(void);
    if (count > MAX_SYNCOBJ) return -EINVAL;
    for (;;) {
        uint64_t wseq = waitqueue_seq();
        uint32_t ndone = 0, first = ~0u;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t h;
            uint64_t p = 0;
            bool bad = false;
            if (copy_from_user(&h, (void *)(handles + 4ull * i), 4)) return -EFAULT;
            if (points && copy_from_user(&p, (void *)(points + 8ull * i), 8)) return -EFAULT;
            if (sync_done(c, h, p, &bad)) {
                if (bad) return -EINVAL;
                ndone++;
                if (first == ~0u) first = i;
            }
        }
        if ((flags & 1) ? ndone == count : ndone > 0) {    /* WAIT_ALL, or any */
            if (first != ~0u && copy_to_user((void *)(argp + first_off), &first, 4)) return -EFAULT;
            return 0;
        }
        if ((int64_t)sched_clock_ns() >= timeout_ns) return -ETIME;
        {
            static uint64_t said;
            uint64_t now = sched_clock_ns();
            if (now - said > 1000000000ull) {         /* (a line a second at most) */
                said = now;
                uint32_t h = 0;
                uint64_t p = 0;
                copy_from_user(&h, (void *)handles, 4);
                if (points) copy_from_user(&p, (void *)points, 8);
                say("SYNCOBJ_WAIT still waiting: handle ", h);
                say("  point ", p);
                if (h && h <= MAX_SYNCOBJ) {
                    say("  signaled ", c->sync[h - 1].signaled);
                    say("  fence engine ", c->sync[h - 1].fcls);
                    say("  fence number ", c->sync[h - 1].fnum);
                    say("  that engine done ", g_q[c->sync[h - 1].fcls & 1].done);
                    say("  reached point ", c->sync[h - 1].point);
                }
                say("  count ", count);
                say("  flags ", flags);
            }
        }
        waitqueue_wait_ms_seq(&g_q[0].done_wq, timer_get_ms() + 2, wseq);   /* (the render engine's ends wake it) */
    }
}

static int64_t syncobj(Client *c, uint32_t nr, uint64_t argp)
{
    switch (nr) {
    case 0xBF: {                                      /* CREATE { handle, flags } */
        uint32_t a[2];
        if (copy_from_user(a, (void *)argp, 8)) return -EFAULT;
        for (uint32_t i = 0; i < MAX_SYNCOBJ; i++) {
            if (c->sync[i].used) continue;
            c->sync[i].used = true;
            c->sync[i].signaled = (a[1] & 1) != 0;    /* CREATE_SIGNALED */
            c->sync[i].fnum = 0;
            c->sync[i].point = 0;
            c->sync[i].pnum = c->sync[i].ppoint = 0;
            a[0] = i + 1;
            return copy_to_user((void *)argp, a, 8) ? -EFAULT : 0;
        }
        return -ENOMEM;
    }
    case 0xC0: {                                      /* DESTROY { handle, pad } */
        uint32_t a[2];
        if (copy_from_user(a, (void *)argp, 8)) return -EFAULT;
        if (!a[0] || a[0] > MAX_SYNCOBJ || !c->sync[a[0] - 1].used) return -EINVAL;
        c->sync[a[0] - 1].used = false;
        return 0;
    }
    case 0xC3: {                                      /* WAIT { handles, timeout, count, flags, first, pad, deadline } */
        struct { uint64_t handles; int64_t timeout; uint32_t count, flags, first, pad; } w;
        if (copy_from_user(&w, (void *)argp, sizeof w)) return -EFAULT;
        return sync_wait(c, w.handles, 0, w.count, w.flags, w.timeout, argp, 24);
    }
    case 0xCA: {                                      /* TIMELINE_WAIT { handles, points, timeout, count, flags, first, pad } */
        struct { uint64_t handles, points; int64_t timeout; uint32_t count, flags, first, pad; } w;
        if (copy_from_user(&w, (void *)argp, sizeof w)) return -EFAULT;
        return sync_wait(c, w.handles, w.points, w.count, w.flags, w.timeout, argp, 32);
    }
    case 0xC4: case 0xC5: {                           /* RESET, SIGNAL { handles, count, pad } */
        struct { uint64_t handles; uint32_t count, pad; } a;
        if (copy_from_user(&a, (void *)argp, sizeof a)) return -EFAULT;
        for (uint32_t i = 0; i < a.count; i++) {
            uint32_t h;
            if (copy_from_user(&h, (void *)(a.handles + 4ull * i), 4)) return -EFAULT;
            if (!h || h > MAX_SYNCOBJ || !c->sync[h - 1].used) return -EINVAL;
            c->sync[h - 1].signaled = nr == 0xC5;
            c->sync[h - 1].fnum = 0;
        }
        return 0;
    }
    case 0xCB: case 0xCD: {                           /* QUERY, TIMELINE_SIGNAL { handles, points, count, flags } */
        struct { uint64_t handles, points; uint32_t count, flags; } a;
        if (copy_from_user(&a, (void *)argp, sizeof a)) return -EFAULT;
        for (uint32_t i = 0; i < a.count; i++) {
            uint32_t h;
            uint64_t p = 0;
            if (copy_from_user(&h, (void *)(a.handles + 4ull * i), 4)) return -EFAULT;
            if (!h || h > MAX_SYNCOBJ || !c->sync[h - 1].used) return -EINVAL;
            if (nr == 0xCB) {
                p = c->sync[h - 1].point;
                if (copy_to_user((void *)(a.points + 8ull * i), &p, 8)) return -EFAULT;
            } else {
                if (copy_from_user(&p, (void *)(a.points + 8ull * i), 8)) return -EFAULT;
                sync_signal(c, h, p);
            }
        }
        return 0;
    }
    case 0xCC: {                                      /* TRANSFER { src, dst, src_point, dst_point, flags, pad } */
        struct { uint32_t src, dst; uint64_t src_point, dst_point; uint32_t flags, pad; } t;
        if (copy_from_user(&t, (void *)argp, sizeof t)) return -EFAULT;
        bool bad = false;
        bool done = sync_done(c, t.src, t.src_point, &bad);
        if (bad || !t.dst || t.dst > MAX_SYNCOBJ || !c->sync[t.dst - 1].used) return -EINVAL;
        if (done) sync_signal(c, t.dst, t.dst_point);
        else if (!t.dst_point) { c->sync[t.dst - 1].signaled = false; c->sync[t.dst - 1].fnum = 0; }
        return 0;
    }
    default:
        say("SYNCOBJ ioctl not done: nr ", nr);
        return -EINVAL;
    }
}

/* ---- sharing: PRIME (dma-buf fds), and the display's view of an object ---- */

extern int      drm_install_prime_fd(uint32_t idx, uint64_t size, bool cloexec);
extern uint32_t drm_prime_fd_bo(int fd);

/* a dma-buf fd names object b as (b | I915_CLIENT_BIT): the DRM layer's
   own objects have no such bit (drivers/video/virtgpu_drm.c) */
static int64_t prime_to_fd(Client *c, uint64_t argp)
{
    struct { uint32_t handle, flags; int32_t fd; } ph;
    if (copy_from_user(&ph, (void *)argp, sizeof ph)) return -EFAULT;
    Bo *bo = bo_of(c, ph.handle);
    if (!bo) return -ENOENT;
    uint32_t b = c->bo[ph.handle - 1];
    int fd = drm_install_prime_fd(b | I915_CLIENT_BIT, bo->size, (ph.flags & 02000000) != 0);
    if (fd < 0) return fd;
    bo->refs++;                                       /* the fd's own reference */
    ph.fd = fd;
    return copy_to_user((void *)argp, &ph, sizeof ph) ? -EFAULT : 0;
}

static int64_t prime_from_fd(Client *c, uint64_t argp)
{
    struct { uint32_t handle, flags; int32_t fd; } ph;
    if (copy_from_user(&ph, (void *)argp, sizeof ph)) return -EFAULT;
    uint32_t idx = drm_prime_fd_bo(ph.fd);
    if (!(idx & I915_CLIENT_BIT)) return -EINVAL;     /* not an Intel GPU object */
    uint32_t b = idx & ~I915_CLIENT_BIT;
    if (!b || b > MAX_BOS || !g_bos[b - 1].used) return -EINVAL;
    for (uint32_t i = 0; i < MAX_HANDLES; i++)        /* known here already: the same handle */
        if (c->bo[i] == b) { ph.handle = i + 1; return copy_to_user((void *)argp, &ph, sizeof ph) ? -EFAULT : 0; }
    for (uint32_t i = 0; i < MAX_HANDLES; i++) {
        if (c->bo[i]) continue;
        c->bo[i] = b;
        c->map_vm[i] = 0;
        g_bos[b - 1].refs++;
        ph.handle = i + 1;
        return copy_to_user((void *)argp, &ph, sizeof ph) ? -EFAULT : 0;
    }
    return -ENOMEM;
}

void i915_prime_get(uint32_t idx)
{
    uint32_t b = idx & ~I915_CLIENT_BIT;
    if (b && b <= MAX_BOS && g_bos[b - 1].used) g_bos[b - 1].refs++;
}

void i915_prime_put(uint32_t idx)
{
    uint32_t b = idx & ~I915_CLIENT_BIT;
    if (b && b <= MAX_BOS && g_bos[b - 1].used) bo_unref(&g_bos[b - 1]);
}

int i915_prime_mmap(uint32_t idx, uint64_t offset, uint64_t len, uint64_t *phys)
{
    uint32_t b = idx & ~I915_CLIENT_BIT;
    if (!b || b > MAX_BOS || !g_bos[b - 1].used) return -EINVAL;
    Bo *bo = &g_bos[b - 1];
    if ((offset & 4095) || offset > bo->size || len > bo->size - offset) return -EINVAL;
    *phys = bo->phys + offset;
    return 0;
}

/* for the display (simpledrm, drivers/video/virtgpu_drm.c): an object a
   framebuffer is made of -- its memory, size, tiling -- and a reference
   for as long as the framebuffer lives */
int i915_bo_info(uint32_t slot, uint32_t handle, uint64_t *phys, uint64_t *size, uint32_t *tiling, uint32_t *ref)
{
    Client *c = client(slot);
    Bo *bo = c ? bo_of(c, handle) : NULL;
    if (!bo) return -ENOENT;
    *phys = bo->phys;
    *size = bo->size;
    *tiling = bo->tiling;
    *ref = c->bo[handle - 1];
    bo->refs++;
    return 0;
}

void i915_bo_unref(uint32_t ref)
{
    if (ref && ref <= MAX_BOS && g_bos[ref - 1].used) bo_unref(&g_bos[ref - 1]);
}

/* dumb buffers on card0 (KMS clients drawing with the CPU): our objects */
int64_t i915_dumb(uint32_t slot, uint32_t nr, uint64_t argp)
{
    Client *c = client(slot);
    if (!c) return -ENODEV;
    if (nr == 0xB2) {                                 /* CREATE_DUMB { height, width, bpp, flags, handle, pitch, size } */
        struct { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; } d;
        if (copy_from_user(&d, (void *)argp, sizeof d)) return -EFAULT;
        if (!d.width || !d.height || !d.bpp) return -EINVAL;
        d.pitch = ((d.width * ((d.bpp + 7) / 8)) + 63) & ~63u;
        uint32_t h = 0;
        int64_t r = bo_new(c, (uint64_t)d.pitch * d.height, &h);
        if (r) return r;
        d.handle = h;
        d.size = g_bos[c->bo[h - 1] - 1].size;
        return copy_to_user((void *)argp, &d, sizeof d) ? -EFAULT : 0;
    }
    if (nr == 0xB3) {                                 /* MAP_DUMB { handle, pad, offset } */
        struct { uint32_t handle, pad; uint64_t offset; } m;
        if (copy_from_user(&m, (void *)argp, sizeof m)) return -EFAULT;
        if (!bo_of(c, m.handle)) return -ENOENT;
        m.offset = (uint64_t)m.handle << 32;
        return copy_to_user((void *)argp, &m, sizeof m) ? -EFAULT : 0;
    }
    if (nr == 0xB4) {                                 /* DESTROY_DUMB { handle } */
        uint32_t h;
        if (copy_from_user(&h, (void *)argp, 4)) return -EFAULT;
        if (!bo_of(c, h)) return -ENOENT;
        handle_close(c, h);
        return 0;
    }
    return -EINVAL;
}

/* ---- the ioctls ---------------------------------------------------------- */

int64_t i915_ioctl(uint32_t slot, uint64_t request, uint64_t argp)
{
    Client *c = client(slot);
    if (!c) return -ENODEV;
    if (((request >> 8) & 0xFF) != 'd') return -ENOTTY;
    uint32_t nr = request & 0xFF;
    switch (nr) {
    case 0x00: {                                      /* VERSION */
        struct { int32_t major, minor, patch; uint32_t pad; uint64_t name_len, name, date_len, date, desc_len, desc; } v;
        if (copy_from_user(&v, (void *)argp, sizeof v)) return -EFAULT;
        static const char name[] = "i915", date[] = "20201103", desc[] = "Intel Graphics (WynlandOS)";
        v.major = 1; v.minor = 6; v.patch = 0;
        if (v.name && v.name_len) copy_to_user((void *)v.name, name, v.name_len < 4 ? v.name_len : 4);
        if (v.date && v.date_len) copy_to_user((void *)v.date, date, v.date_len < 8 ? v.date_len : 8);
        if (v.desc && v.desc_len) copy_to_user((void *)v.desc, desc, v.desc_len < sizeof desc - 1 ? v.desc_len : sizeof desc - 1);
        v.name_len = 4; v.date_len = 8; v.desc_len = sizeof desc - 1;
        return copy_to_user((void *)argp, &v, sizeof v) ? -EFAULT : 0;
    }
    case 0x01: return 0;                              /* GET_UNIQUE */
    case 0x0C: {                                      /* GET_CAP { capability, value } */
        uint64_t cap[2];
        if (copy_from_user(cap, (void *)argp, sizeof cap)) return -EFAULT;
        switch (cap[0]) {
        case 0x5:  cap[1] = 3; break;                 /* PRIME: import | export */
        case 0x6:  cap[1] = 1; break;                 /* TIMESTAMP_MONOTONIC */
        case 0x13: cap[1] = 1; break;                 /* SYNCOBJ */
        case 0x14: cap[1] = 1; break;                 /* SYNCOBJ_TIMELINE */
        default:   cap[1] = 0; break;
        }
        return copy_to_user((void *)argp, cap, sizeof cap) ? -EFAULT : 0;
    }
    case 0x0D: return 0;                              /* SET_CLIENT_CAP */
    case 0x2D: return prime_to_fd(c, argp);           /* PRIME_HANDLE_TO_FD */
    case 0x2E: return prime_from_fd(c, argp);         /* PRIME_FD_TO_HANDLE */
    case 0x09: {                                      /* GEM_CLOSE { handle, pad } */
        uint32_t a[2];
        if (copy_from_user(a, (void *)argp, 8)) return -EFAULT;
        if (!bo_of(c, a[0])) return -EINVAL;
        handle_close(c, a[0]);
        return 0;
    }
    case 0xBF: case 0xC0: case 0xC3: case 0xC4: case 0xC5:
    case 0xCA: case 0xCB: case 0xCC: case 0xCD:
        return syncobj(c, nr, argp);
    default: break;
    }
    if (nr < 0x40 || nr > 0x9F) { say("ioctl not known: nr ", nr); return -EINVAL; }
    switch (nr - 0x40) {
    case 0x06: return getparam(argp);                 /* GETPARAM */
    case 0x1b: case 0x3c: {                           /* GEM_CREATE, GEM_CREATE_EXT { size, handle, flags, extensions } */
        uint64_t a[3];
        if (copy_from_user(a, (void *)argp, 16)) return -EFAULT;
        uint32_t h = 0;
        int64_t r = bo_new(c, a[0], &h);
        if (r) return r;
        a[0] = g_bos[c->bo[h - 1] - 1].size;
        ((uint32_t *)a)[2] = h;
        return copy_to_user((void *)argp, a, 16) ? -EFAULT : 0;
    }
    case 0x24: {                                      /* GEM_MMAP_GTT, and (0x24 too) MMAP_OFFSET { handle, pad, offset, flags } */
        uint32_t a[2];
        if (copy_from_user(a, (void *)argp, 8)) return -EFAULT;
        if (!bo_of(c, a[0])) return -ENOENT;
        uint64_t off = (uint64_t)a[0] << 32;
        return copy_to_user((void *)(argp + 8), &off, 8) ? -EFAULT : 0;
    }
    case 0x1f: {                                      /* SET_DOMAIN { handle, read, write }: the CPU after the GPU */
        uint32_t a[3];
        if (copy_from_user(a, (void *)argp, 12)) return -EFAULT;
        Bo *bo = bo_of(c, a[0]);
        if (!bo) return -ENOENT;
        bo_wait(bo, 3000);
        return 0;
    }
    case 0x20: case 0x18: return 0;                   /* SW_FINISH, THROTTLE */
    case 0x17: {                                      /* GEM_BUSY { handle, busy } */
        uint32_t a[2];
        if (copy_from_user(a, (void *)argp, 8)) return -EFAULT;
        Bo *bo = bo_of(c, a[0]);
        if (!bo) return -ENOENT;
        a[1] = bo_idle(bo) ? 0 : (bo->last[0] > g_q[0].done ? 0x10001u : 0x20002u);   /* read engines << 16 | writer */
        return copy_to_user((void *)argp, a, 8) ? -EFAULT : 0;
    }
    case 0x2c: {                                      /* GEM_WAIT { handle, flags, timeout_ns } */
        struct { uint32_t handle, flags; int64_t timeout; } w;
        if (copy_from_user(&w, (void *)argp, sizeof w)) return -EFAULT;
        Bo *bo = bo_of(c, w.handle);
        if (!bo) return -ENOENT;
        if (bo_idle(bo)) return 0;
        if (w.timeout == 0) return -ETIME;
        uint64_t ms = w.timeout < 0 ? 0xFFFFFFFFull : (uint64_t)w.timeout / 1000000 + 1;
        if (!bo_wait(bo, ms)) return -ETIME;
        return 0;
    }
    case 0x26: {                                      /* GEM_MADVISE { handle, madv, retained } */
        uint32_t a[3];
        if (copy_from_user(a, (void *)argp, 12)) return -EFAULT;
        if (!bo_of(c, a[0])) return -ENOENT;
        a[2] = 1;
        return copy_to_user((void *)argp, a, 12) ? -EFAULT : 0;
    }
    case 0x21: case 0x22: {                           /* SET_TILING, GET_TILING { handle, mode, stride, swizzle } */
        uint32_t a[4];
        if (copy_from_user(a, (void *)argp, 16)) return -EFAULT;
        Bo *bo = bo_of(c, a[0]);
        if (!bo) return -ENOENT;
        if (nr - 0x40 == 0x21) { bo->tiling = a[1]; bo->stride = a[2]; a[3] = 0; }
        else { a[1] = bo->tiling; a[2] = 0; a[3] = 0; }
        return copy_to_user((void *)argp, a, 16) ? -EFAULT : 0;
    }
    case 0x2f: case 0x30: {                           /* SET_CACHING, GET_CACHING { handle, caching } */
        uint32_t a[2];
        if (copy_from_user(a, (void *)argp, 8)) return -EFAULT;
        Bo *bo = bo_of(c, a[0]);
        if (!bo) return -ENOENT;
        if (nr - 0x40 == 0x2f) bo->caching = a[1]; else a[1] = bo->caching;
        return copy_to_user((void *)argp, a, 8) ? -EFAULT : 0;
    }
    case 0x23: {                                      /* GEM_GET_APERTURE */
        uint64_t a[2] = { 256ull << 20, 256ull << 20 };
        return copy_to_user((void *)argp, a, 16) ? -EFAULT : 0;
    }
    case 0x31: {                                      /* REG_READ { offset, val }: the render timestamp */
        uint64_t a[2];
        if (copy_from_user(a, (void *)argp, 16)) return -EFAULT;
        if ((a[0] & ~1ull) != 0x2358) { say("REG_READ of a register not offered: ", a[0]); return -EINVAL; }
        if (!i915_fw_get()) return -ENODEV;
        uint32_t hi, lo, hi2;
        do { hi = i915_rd(0x235C); lo = i915_rd(0x2358); hi2 = i915_rd(0x235C); } while (hi != hi2);
        i915_fw_put();
        a[1] = ((uint64_t)hi << 32) | lo;
        return copy_to_user((void *)argp, a, 16) ? -EFAULT : 0;
    }
    case 0x32: {                                      /* GET_RESET_STATS { ctx_id, flags, reset_count, batch_active, batch_pending, pad } */
        uint32_t a[6];
        if (copy_from_user(a, (void *)argp, sizeof a)) return -EFAULT;
        if (a[0] >= MAX_CTXS || !c->ctxs[a[0]].used) return -ENOENT;
        a[2] = g_resets;                              /* resets of the GPU */
        a[3] = c->ctxs[a[0]].hangs;                   /* batches of this context that hung it */
        a[4] = 0;
        return copy_to_user((void *)argp, a, sizeof a) ? -EFAULT : 0;
    }
    case 0x2d: return ctx_create(c, argp);            /* CONTEXT_CREATE(_EXT): the same number */
    case 0x2e: {                                      /* CONTEXT_DESTROY { ctx_id, pad } */
        uint32_t a[2];
        if (copy_from_user(a, (void *)argp, 8)) return -EFAULT;
        if (!a[0] || a[0] >= MAX_CTXS || !c->ctxs[a[0]].used) return -ENOENT;
        client_idle(c);                               /* (queued requests use it) */
        c->ctxs[a[0]].used = false;                   /* (its hardware contexts' pages kept for now) */
        return 0;
    }
    case 0x34: return ctx_param(c, argp, false);      /* CONTEXT_GETPARAM */
    case 0x35: return ctx_param(c, argp, true);       /* CONTEXT_SETPARAM */
    case 0x39: return query(argp);                    /* QUERY */
    case 0x3a: {                                      /* VM_CREATE { extensions, flags, vm_id } */
        uint32_t a[4];
        if (copy_from_user(a, (void *)argp, 16)) return -EFAULT;
        uint32_t id;
        if (!vm_new(c, &id)) return -ENOMEM;
        a[3] = id;
        return copy_to_user((void *)argp, a, 16) ? -EFAULT : 0;
    }
    case 0x3b: return 0;                              /* VM_DESTROY (kept until the client goes) */
    case 0x29: return execbuffer(c, argp);            /* GEM_EXECBUFFER2(_WR) */
    default:
        say("i915 ioctl not known: DRM_COMMAND_BASE + ", nr - 0x40);
        return -EINVAL;
    }
}
