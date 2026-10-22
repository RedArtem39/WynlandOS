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
    GpuCtx   hw[2];                                   /* per engine: RCS, BCS */
    bool     hw_made[2];
} Ctx;

typedef struct {
    bool     used;
    uint32_t refs;
    uint32_t bo[MAX_HANDLES];                         /* handle - 1 -> object index + 1 */
    Vm       vms[MAX_VMS];
    Ctx      ctxs[MAX_CTXS];                          /* context 0: the default one */
    uint8_t  syncobj[MAX_SYNCOBJ];                    /* 0 free, 1 unsignaled, 2 signaled */
    /* where each object is mapped in this client's VMs (one place each) */
    uint64_t map_va[MAX_HANDLES];
    uint32_t map_vm[MAX_HANDLES];                     /* VM index + 1; 0: not mapped */
} Client;

static Bo g_bos[MAX_BOS];
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

static void handle_close(Client *c, uint32_t h)
{
    Bo *bo = bo_of(c, h);
    if (!bo) return;
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
    for (uint32_t h = 1; h <= MAX_HANDLES; h++) handle_close(c, h);
    g_clients[(slot & ~I915_CLIENT_BIT) - 1] = NULL;
    c->used = false;
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
    case 55: v = 0; break;                            /* HAS_EXEC_TIMELINE_FENCES: not yet */
    case 56: v = 0; break;                            /* HAS_USERPTR_PROBE */
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
    Vm *v = &c->vms[x->vm - 1];
    /* the engine: an index into the context's map, or the legacy ring */
    uint32_t sel = (uint32_t)(eb.flags & 0x3F), cls;
    if (x->nengines) {
        if (sel >= x->nengines) return -EINVAL;
        cls = x->engine_class[sel];
    } else {
        cls = (sel == 3) ? 1 : 0;                     /* I915_EXEC_BLT, else render */
    }

    /* the objects: each at its address in the VM (softpin) */
    struct exo { uint32_t handle, relocation_count; uint64_t relocs_ptr, alignment, offset, flags, rsvd1, rsvd2; };
    uint64_t batch_va = 0;
    uint32_t batch_idx = (eb.flags & (1u << 18)) ? 0 : eb.buffer_count - 1;   /* BATCH_FIRST */
    for (uint32_t i = 0; i < eb.buffer_count; i++) {
        struct exo o;
        if (copy_from_user(&o, (void *)(eb.buffers_ptr + (uint64_t)i * sizeof o), sizeof o)) return -EFAULT;
        uint32_t h = o.handle;
        if (eb.flags & (1u << 12)) { /* HANDLE_LUT: the handle is as given (no LUT of our own) */ }
        Bo *bo = bo_of(c, h);
        if (!bo) { say("execbuffer: no object, handle ", h); return -ENOENT; }
        if (o.relocation_count) { say("execbuffer: relocations are not done, handle ", h); return -EINVAL; }
        if (!(o.flags & (1u << 4))) {                 /* not pinned: its place now, at the next free one */
            static uint64_t next = 0x100000000ull;
            if (!c->map_vm[h - 1]) { o.offset = next; next += bo->size + 4096; }
            else o.offset = c->map_va[h - 1];
            if (copy_to_user((void *)(eb.buffers_ptr + (uint64_t)i * sizeof o + 24), &o.offset, 8)) return -EFAULT;
        }
        uint64_t va = o.offset & ((1ull << 48) - 1);
        if (c->map_vm[h - 1] != x->vm || c->map_va[h - 1] != va) {
            if (c->map_vm[h - 1]) i915_vm_unmap(&c->vms[c->map_vm[h - 1] - 1].vm, c->map_va[h - 1], (uint32_t)(bo->size / 4096));
            if (!i915_vm_map(&v->vm, va, bo->phys, (uint32_t)(bo->size / 4096), 0)) return -ENOMEM;
            c->map_vm[h - 1] = x->vm;
            c->map_va[h - 1] = va;
        }
        if (i == batch_idx) batch_va = va + eb.batch_start_offset;
    }

    /* the hardware context for that engine, in the context's VM */
    Engine *e = cls ? &i915_bcs : &i915_rcs;
    if (!x->hw_made[cls]) {
        if (!i915_ctx_create(&x->hw[cls], e, v->vm.pml4.phys)) return -ENOMEM;
        x->hw[cls].inval = (cls == 1);
        x->hw_made[cls] = true;
    }
    uint32_t cmds[24], n = 0;
    if (cls == 0) {
        /* PIPE_CONTROL: caches and TLB invalidated before the batch (a
           post-sync write with it: into the status page) */
        cmds[n++] = 0x7A000004;
        cmds[n++] = (1u << 20) | (1u << 18) | (1u << 11) | (1u << 10) | (1u << 4) | (1u << 3) | (1u << 2) |
                    (1u << 14) | (1u << 24);          /* CS stall, TLB, instr, texture, VF, const, state; write imm, GGTT */
        cmds[n++] = e->hwsp.ggtt + 0x900; cmds[n++] = 0; cmds[n++] = 0; cmds[n++] = 0;
    }
    cmds[n++] = 0x18800101;                           /* MI_BATCH_BUFFER_START, PPGTT */
    cmds[n++] = (uint32_t)batch_va;
    cmds[n++] = (uint32_t)(batch_va >> 32);
    if (cls == 0) {
        /* PIPE_CONTROL: render target, depth, data port, tile caches
           flushed (Gen12: the HDC pipeline too, a bit of dword 0) */
        cmds[n++] = 0x7A000004 | (1u << 9);
        cmds[n++] = (1u << 20) | (1u << 12) | (1u << 0) | (1u << 5) | (1u << 28) | (1u << 14) | (1u << 24);
        cmds[n++] = e->hwsp.ggtt + 0x900; cmds[n++] = 0; cmds[n++] = 0; cmds[n++] = 0;
    } else {
        cmds[n++] = 0x13000002; cmds[n++] = 0; cmds[n++] = 0; cmds[n++] = 0;   /* MI_FLUSH_DW */
    }
    if (!i915_fw_get()) return -ENODEV;
    uint32_t seq = i915_ctx_submit(&x->hw[cls], cmds, n);
    bool done = i915_wait_seqno(e, seq, 3000);
    i915_fw_put();
    if (!done) {
        say("execbuffer: the GPU did not finish in 3 s, engine ", e->base);
        say("  ACTHD ", i915_rd(e->base + 0x74));
        say("  IPEHR ", i915_rd(e->base + 0x68));
        i915_engine_reset(e);
        x->hw_made[cls] = false;                      /* a fresh context next time */
        return -5;                                    /* -EIO: Mesa takes it as a GPU hang */
    }
    /* fences: every syncobj named is signaled already (synchronous) */
    if ((eb.flags & (1u << 19)) && eb.num_cliprects && eb.cliprects_ptr) {
        for (uint32_t i = 0; i < eb.num_cliprects; i++) {
            uint32_t f[2];
            if (copy_from_user(f, (void *)(eb.cliprects_ptr + 8ull * i), 8)) return -EFAULT;
            if ((f[1] & 2) && f[0] && f[0] <= MAX_SYNCOBJ && c->syncobj[f[0] - 1]) c->syncobj[f[0] - 1] = 2;
        }
    }
    return 0;
}

/* ---- syncobjs (all signaled at submission: execbuffer is synchronous) ----- */

static int64_t syncobj(Client *c, uint32_t nr, uint64_t argp)
{
    switch (nr) {
    case 0xBF: {                                      /* CREATE { handle, flags } */
        uint32_t a[2];
        if (copy_from_user(a, (void *)argp, 8)) return -EFAULT;
        for (uint32_t i = 0; i < MAX_SYNCOBJ; i++) {
            if (c->syncobj[i]) continue;
            c->syncobj[i] = (a[1] & 1) ? 2 : 1;       /* CREATE_SIGNALED */
            a[0] = i + 1;
            return copy_to_user((void *)argp, a, 8) ? -EFAULT : 0;
        }
        return -ENOMEM;
    }
    case 0xC0: {                                      /* DESTROY { handle, pad } */
        uint32_t a[2];
        if (copy_from_user(a, (void *)argp, 8)) return -EFAULT;
        if (!a[0] || a[0] > MAX_SYNCOBJ || !c->syncobj[a[0] - 1]) return -EINVAL;
        c->syncobj[a[0] - 1] = 0;
        return 0;
    }
    case 0xC3:                                        /* WAIT */
    case 0xCA:                                        /* TIMELINE_WAIT */
        return 0;                                     /* nothing is ever pending */
    case 0xC4: case 0xC5: {                           /* RESET, SIGNAL { handles, count, pad } */
        struct { uint64_t handles; uint32_t count, pad; } a;
        if (copy_from_user(&a, (void *)argp, sizeof a)) return -EFAULT;
        for (uint32_t i = 0; i < a.count; i++) {
            uint32_t h;
            if (copy_from_user(&h, (void *)(a.handles + 4ull * i), 4)) return -EFAULT;
            if (h && h <= MAX_SYNCOBJ && c->syncobj[h - 1]) c->syncobj[h - 1] = nr == 0xC5 ? 2 : 1;
        }
        return 0;
    }
    default:
        say("SYNCOBJ ioctl not done: nr ", nr);
        return -EINVAL;
    }
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
        case 0x5:  cap[1] = 3; break;                 /* PRIME: import | export (not yet: see below) */
        case 0x6:  cap[1] = 1; break;                 /* TIMESTAMP_MONOTONIC */
        case 0x13: cap[1] = 1; break;                 /* SYNCOBJ */
        case 0x14: cap[1] = 0; break;                 /* SYNCOBJ_TIMELINE */
        default:   cap[1] = 0; break;
        }
        return copy_to_user((void *)argp, cap, sizeof cap) ? -EFAULT : 0;
    }
    case 0x0D: return 0;                              /* SET_CLIENT_CAP */
    case 0x09: {                                      /* GEM_CLOSE { handle, pad } */
        uint32_t a[2];
        if (copy_from_user(a, (void *)argp, 8)) return -EFAULT;
        if (!bo_of(c, a[0])) return -EINVAL;
        handle_close(c, a[0]);
        return 0;
    }
    case 0xBF: case 0xC0: case 0xC3: case 0xC4: case 0xC5: case 0xCA:
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
    case 0x1f: case 0x20: case 0x18: return 0;        /* SET_DOMAIN, SW_FINISH, THROTTLE */
    case 0x17: {                                      /* GEM_BUSY { handle, busy } */
        uint32_t a[2];
        if (copy_from_user(a, (void *)argp, 8)) return -EFAULT;
        if (!bo_of(c, a[0])) return -ENOENT;
        a[1] = 0;
        return copy_to_user((void *)argp, a, 8) ? -EFAULT : 0;
    }
    case 0x2c: {                                      /* GEM_WAIT { handle, flags, timeout } */
        uint32_t a[2];
        if (copy_from_user(a, (void *)argp, 8)) return -EFAULT;
        return bo_of(c, a[0]) ? 0 : -ENOENT;
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
        a[2] = a[3] = a[4] = 0;
        return copy_to_user((void *)argp, a, sizeof a) ? -EFAULT : 0;
    }
    case 0x2d: return ctx_create(c, argp);            /* CONTEXT_CREATE(_EXT): the same number */
    case 0x2e: {                                      /* CONTEXT_DESTROY { ctx_id, pad } */
        uint32_t a[2];
        if (copy_from_user(a, (void *)argp, 8)) return -EFAULT;
        if (!a[0] || a[0] >= MAX_CTXS || !c->ctxs[a[0]].used) return -ENOENT;
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
