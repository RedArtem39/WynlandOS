/*
 * WynlandOS - DRM render node for virtio-gpu 3D (virgl)
 * ============================================================
 * Implements the subset of the Linux virtio_gpu DRM uAPI that Mesa's virgl
 * winsys (src/gallium/winsys/virgl/drm/virgl_drm_winsys.c) uses:
 *
 *   GETPARAM, GET_CAPS, RESOURCE_CREATE, RESOURCE_INFO, MAP (+ mmap),
 *   EXECBUFFER, TRANSFER_TO_HOST, TRANSFER_FROM_HOST, WAIT, GEM_CLOSE
 *   plus the generic DRM_IOCTL_VERSION / GET_CAP.
 *
 * Design:
 * - One DrmClient per open() of the node (VfsFile.current_cluster holds its
 *   slot + 1). Its virgl context is created lazily on first 3D use, like
 *   the Linux driver does without VIRTGPU_PARAM_CONTEXT_INIT.
 * - A buffer object (BO) is one host resource plus one physically
 *   contiguous guest backing range, attached with a single mem entry and
 *   mapped into user space with PAGE_SHARED_MAP (the BO owns the pages).
 * - Every host operation touching a BO is fenced; the fenced request's
 *   completion is our "GPU is done with it" signal (QEMU answers a fenced
 *   command only when its virgl fence retires). A BO remembers its last
 *   ticket; WAIT sleeps until that ticket is done.
 * - We report DRM version 0.0, so Mesa uses no sync_file fences and waits
 *   through WAIT on its fence BOs instead -- no dma-fence/sync_file needed.
 * - Commands whose buffers must outlive the syscall (fenced submits,
 *   transfers) are heap-allocated and parked on a pending list, freed once
 *   their ticket completes.
 *
 * Runs in syscall context (interrupts off, uniprocessor): no locking.
 */
#include <wynland/types.h>
#include <wynland/virtio_gpu.h>
#include <wynland/virtgpu_drm.h>
#include <wynland/pmm.h>
#include <wynland/vmm.h>
#include <wynland/heap.h>
#include <wynland/usercopy.h>
#include <wynland/sched.h>
#include <wynland/process.h>

extern void serial_write_string(const char *str);
extern uint64_t timer_get_ms(void);
extern void *pmm_alloc_contiguous(uint32_t count);

#define PAGE_SZ 4096u

/* 1 = log every ioctl (nr, result) and host-side failures to serial */
#ifndef DRM_DEBUG
#define DRM_DEBUG 0
#endif

static void dbg_num(const char *pre, int64_t v)
{
#if DRM_DEBUG
    extern void uint_to_hex(uint64_t val, char *buf);
    char b[32];
    serial_write_string(pre);
    if (v < 0) { serial_write_string("-"); v = -v; }
    uint_to_hex((uint64_t)v, b);
    serial_write_string(b);
    serial_write_string("\r\n");
#else
    (void)pre; (void)v;
#endif
}

#define EPERM   1
#define ENOENT  2
#define EINTR   4
#define EFAULT  14
#define EBUSY   16
#define ENODEV  19
#define EINVAL  22
#define ENOMEM  12
#define ENOSPC  28
#define ENOTTY  25
#define ETIMEDOUT 110

/* ---- uAPI (include/uapi/drm/virtgpu_drm.h, drm.h) ---- */

#define DRM_IOCTL_TYPE          0x64 /* 'd' */
#define DRM_COMMAND_BASE        0x40

#define DRM_NR_VERSION          0x00
#define DRM_NR_GET_UNIQUE       0x01
#define DRM_NR_GEM_CLOSE        0x09
#define DRM_NR_GET_CAP          0x0c
#define DRM_NR_SET_CLIENT_CAP   0x0d

#define VIRTGPU_MAP             0x01
#define VIRTGPU_EXECBUFFER      0x02
#define VIRTGPU_GETPARAM        0x03
#define VIRTGPU_RESOURCE_CREATE 0x04
#define VIRTGPU_RESOURCE_INFO   0x05
#define VIRTGPU_TRANSFER_FROM_HOST 0x06
#define VIRTGPU_TRANSFER_TO_HOST   0x07
#define VIRTGPU_WAIT            0x08
#define VIRTGPU_GET_CAPS        0x09
#define VIRTGPU_RESOURCE_CREATE_BLOB 0x0a
#define VIRTGPU_CONTEXT_INIT    0x0b

#define VIRTGPU_PARAM_3D_FEATURES          1
#define VIRTGPU_PARAM_CAPSET_QUERY_FIX     2
#define VIRTGPU_PARAM_RESOURCE_BLOB        3
#define VIRTGPU_PARAM_HOST_VISIBLE         4
#define VIRTGPU_PARAM_CROSS_DEVICE         5
#define VIRTGPU_PARAM_CONTEXT_INIT         6
#define VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs 7

#define VIRTGPU_WAIT_NOWAIT 1

struct drm_version_u {
    int32_t  version_major, version_minor, version_patchlevel;
    uint32_t _pad;
    uint64_t name_len;  uint64_t name;
    uint64_t date_len;  uint64_t date;
    uint64_t desc_len;  uint64_t desc;
};

struct drm_get_cap_u   { uint64_t capability; uint64_t value; };
struct drm_gem_close_u { uint32_t handle; uint32_t pad; };

struct virtgpu_map_u      { uint64_t offset; uint32_t handle; uint32_t pad; };
struct virtgpu_getparam_u { uint64_t param; uint64_t value; };

struct virtgpu_execbuffer_u {
    uint32_t flags;
    uint32_t size;
    uint64_t command;
    uint64_t bo_handles;
    uint32_t num_bo_handles;
    int32_t  fence_fd;
    uint32_t ring_idx;
    uint32_t syncobj_stride;
    uint32_t num_in_syncobjs;
    uint32_t num_out_syncobjs;
    uint64_t in_syncobjs;
    uint64_t out_syncobjs;
};

struct virtgpu_resource_create_u {
    uint32_t target, format, bind, width, height, depth;
    uint32_t array_size, last_level, nr_samples, flags;
    uint32_t bo_handle, res_handle, size, stride;
};

struct virtgpu_resource_info_u { uint32_t bo_handle, res_handle, size, blob_mem; };

struct virtgpu_box_u { uint32_t x, y, z, w, h, d; };

struct virtgpu_transfer_u {
    uint32_t bo_handle;
    struct virtgpu_box_u box;
    uint32_t level;
    uint32_t offset;
    uint32_t stride;
    uint32_t layer_stride;
};

struct virtgpu_wait_u     { uint32_t handle; uint32_t flags; };
struct virtgpu_get_caps_u { uint32_t cap_set_id; uint32_t cap_set_ver; uint64_t addr; uint32_t size; uint32_t pad; };

/* ---- state ---- */

#define DRM_MAX_CLIENTS  32
#define DRM_MAX_BOS      4096
#define DRM_MAX_PENDING  512
#define DRM_MAX_CMDBUF   (1024u * 1024u) /* virgl caps its stream at 256 KB; generous */
#define DRM_WAIT_MS      10000

typedef struct {
    bool     used;
    uint64_t pid;       /* opener */
    uint32_t ctx_id;    /* 0 until the first 3D call */
    bool     orphaned;  /* opener exited; free from the next ioctl */
} DrmClient;

typedef struct {
    bool       used;
    uint32_t   client;  /* owning client slot */
    uint32_t   res_id;
    uint64_t   phys;
    uint32_t   pages;
    uint32_t   size;
    bool       has_last;
    VgpuTicket last;    /* last host request touching this BO */
} DrmBo;

typedef struct {
    void      *mem;
    VgpuTicket t;
} DrmPending;

static DrmClient  g_clients[DRM_MAX_CLIENTS];
static DrmBo      g_bos[DRM_MAX_BOS];       /* GEM handle = index + 1 */
static DrmPending g_pending[DRM_MAX_PENDING];

/* ---- helpers ---- */

static void client_free(uint32_t slot);

static void pending_gc(void)
{
    for (uint32_t i = 0; i < DRM_MAX_CLIENTS; i++) {
        if (g_clients[i].used && g_clients[i].orphaned) client_free(i + 1);
    }
    for (int i = 0; i < DRM_MAX_PENDING; i++) {
        if (g_pending[i].mem && vgpu_done(g_pending[i].t)) {
            kfree(g_pending[i].mem);
            g_pending[i].mem = NULL;
        }
    }
}

/* Sleep (not spin) until `t` is done or `ms` pass. Syscall context. */
static bool wait_ticket(VgpuTicket t, uint32_t ms)
{
    uint64_t deadline = timer_get_ms() + ms;
    while (!vgpu_done(t)) {
        if (timer_get_ms() >= deadline) return false;
        sched_block_ms(NULL, timer_get_ms() + 1);
    }
    return true;
}

/* Park heap memory backing an in-flight request; freed by pending_gc(). */
static bool pending_add(void *mem, VgpuTicket t)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < DRM_MAX_PENDING; i++) {
            if (!g_pending[i].mem) {
                g_pending[i].mem = mem;
                g_pending[i].t = t;
                return true;
            }
        }
        /* Full: wait for the oldest-looking entry, then retry once. */
        wait_ticket(g_pending[0].t, DRM_WAIT_MS);
        pending_gc();
    }
    return false;
}

/* Submit a heap-allocated command (header at the start of `mem`, `cmd_len`
   bytes, then an optional trailing data region, then a response header
   at `resp_off`). Ownership of `mem` passes to the pending list. */
static bool submit_owned(void *mem, uint32_t cmd_len, uint32_t data_off, uint32_t data_len,
                         uint32_t resp_off, VgpuTicket *t)
{
    uint8_t *m = (uint8_t *)mem;
    if (!vgpu_submit(m, cmd_len, data_len ? m + data_off : NULL, data_len,
                     m + resp_off, sizeof(VirtioGpuCtrlResponse), t)) {
        kfree(mem);
        return false;
    }
    if (!pending_add(mem, *t)) {
        /* can't track it: wait it out so the memory is safe to free */
        wait_ticket(*t, DRM_WAIT_MS);
        kfree(mem);
    }
    return true;
}

/* Synchronous command through the same path (small, rare commands). */
static bool send_sync(void *cmd, uint32_t cmd_len, uint32_t *resp_type)
{
    uint8_t *mem = (uint8_t *)kmalloc(cmd_len + sizeof(VirtioGpuCtrlResponse));
    if (!mem) return false;
    memcpy(mem, cmd, cmd_len);
    memset(mem + cmd_len, 0, sizeof(VirtioGpuCtrlResponse));
    VgpuTicket t;
    if (!vgpu_submit(mem, cmd_len, NULL, 0, mem + cmd_len, sizeof(VirtioGpuCtrlResponse), &t)) {
        kfree(mem);
        return false;
    }
    bool ok = wait_ticket(t, DRM_WAIT_MS);
    if (!ok) dbg_num("DRM: sync command timed out, type=", ((VirtioGpuCtrlHeader *)cmd)->type);
    if (ok && resp_type) *resp_type = ((VirtioGpuCtrlResponse *)(mem + cmd_len))->type;
    if (ok) kfree(mem);
    else pending_add(mem, t); /* still owned by the device */
    return ok;
}

static DrmClient *client_get(uint32_t *slot)
{
    if (*slot == 0) {
        for (uint32_t i = 0; i < DRM_MAX_CLIENTS; i++) {
            if (!g_clients[i].used) {
                memset(&g_clients[i], 0, sizeof(g_clients[i]));
                g_clients[i].used = true;
                g_clients[i].pid = sched_current()->proc->pid;
                *slot = i + 1;
                return &g_clients[i];
            }
        }
        return NULL;
    }
    if (*slot > DRM_MAX_CLIENTS || !g_clients[*slot - 1].used) return NULL;
    return &g_clients[*slot - 1];
}

static bool client_ensure_ctx(DrmClient *c)
{
    if (c->ctx_id) return true;
    VirtioGpuCtxCreate cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd.hdr.ctx_id = vgpu_alloc_ctx_id();
    const char *name = "wynland";
    for (int i = 0; name[i]; i++) cmd.debug_name[i] = name[i];
    cmd.nlen = 7;
    uint32_t rt = 0;
    if (!send_sync(&cmd, sizeof(cmd), &rt) || rt != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_write_string("DRM: CTX_CREATE failed\r\n");
        return false;
    }
    c->ctx_id = cmd.hdr.ctx_id;
    return true;
}

static DrmBo *bo_lookup(uint32_t client_slot, uint32_t handle)
{
    if (handle == 0 || handle > DRM_MAX_BOS) return NULL;
    DrmBo *bo = &g_bos[handle - 1];
    if (!bo->used || bo->client != client_slot) return NULL;
    return bo;
}

static void bo_destroy(DrmClient *c, DrmBo *bo)
{
    if (bo->has_last) wait_ticket(bo->last, DRM_WAIT_MS);

    VirtioGpuCtxResource cmd;
    memset(&cmd, 0, sizeof(cmd));
    if (c && c->ctx_id) {
        cmd.hdr.type = VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE;
        cmd.hdr.ctx_id = c->ctx_id;
        cmd.resource_id = bo->res_id;
        send_sync(&cmd, sizeof(cmd), NULL);
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    cmd.resource_id = bo->res_id;
    bool unref_ok = send_sync(&cmd, sizeof(cmd), NULL);

    /* Only reuse the pages once the host has dropped its reference. Any
       user mapping still left is the caller's bug (Mesa munmaps first);
       leaking the frames then is safer than handing them out again. */
    if (unref_ok) {
        for (uint32_t i = 0; i < bo->pages; i++)
            pmm_free_page((void *)(uintptr_t)(bo->phys + (uint64_t)i * PAGE_SZ));
    }
    memset(bo, 0, sizeof(*bo));
}

/* ---- ioctls ---- */

static int64_t ioctl_version(uint64_t argp)
{
    struct drm_version_u v;
    if (copy_from_user(&v, (void *)argp, sizeof(v))) return -EFAULT;
    /* 0.0: Mesa's virgl requires major 0; minor 0 = no sync_file fences. */
    static const char name[] = "virtio_gpu", date[] = "0", desc[] = "WynlandOS virtio-gpu";
    const char *strs[3] = { name, date, desc };
    uint64_t *lens[3] = { &v.name_len, &v.date_len, &v.desc_len };
    uint64_t ptrs[3] = { v.name, v.date, v.desc };
    for (int i = 0; i < 3; i++) {
        uint64_t n = 0;
        while (strs[i][n]) n++;
        /* libdrm calls twice: once with zero lengths to learn them */
        if (ptrs[i] && *lens[i]) {
            uint64_t k = n < *lens[i] ? n : *lens[i];
            if (copy_to_user((void *)ptrs[i], strs[i], k)) return -EFAULT;
        }
        *lens[i] = n;
    }
    v.version_major = 0;
    v.version_minor = 0;
    v.version_patchlevel = 0;
    if (copy_to_user((void *)argp, &v, sizeof(v))) return -EFAULT;
    return 0;
}

static int64_t ioctl_getparam(uint64_t argp)
{
    struct virtgpu_getparam_u gp;
    if (copy_from_user(&gp, (void *)argp, sizeof(gp))) return -EFAULT;
    int value;
    switch (gp.param) {
    case VIRTGPU_PARAM_3D_FEATURES:      value = g_virgl ? 1 : 0; break;
    case VIRTGPU_PARAM_CAPSET_QUERY_FIX: value = 1; break;
    case VIRTGPU_PARAM_RESOURCE_BLOB:    value = 0; break;
    case VIRTGPU_PARAM_HOST_VISIBLE:     value = 0; break;
    case VIRTGPU_PARAM_CROSS_DEVICE:     value = 0; break;
    case VIRTGPU_PARAM_CONTEXT_INIT:     value = 0; break; /* ctx created lazily */
    case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs: {
        value = 0;
        for (uint32_t i = 0; i < virtio_gpu_capset_count(); i++) {
            uint32_t id = virtio_gpu_capset(i)->capset_id;
            if (id < 31) value |= 1 << id;
        }
        break;
    }
    default: return -EINVAL;
    }
    /* Linux copies an int to the user pointer */
    if (copy_to_user((void *)gp.value, &value, sizeof(value))) return -EFAULT;
    return 0;
}

static int64_t ioctl_get_caps(uint64_t argp)
{
    struct virtgpu_get_caps_u gc;
    if (copy_from_user(&gc, (void *)argp, sizeof(gc))) return -EFAULT;
    dbg_num("DRM: GET_CAPS id=", gc.cap_set_id);
    dbg_num("DRM:   ver=", gc.cap_set_ver);
    dbg_num("DRM:   size=", gc.size);
    dbg_num("DRM:   known capsets=", virtio_gpu_capset_count());
    uint32_t max_ver = 0, found = 0;
    for (uint32_t i = 0; i < virtio_gpu_capset_count(); i++) {
        const VirtioGpuCapsetInfo *ci = virtio_gpu_capset(i);
        if (ci->capset_id == gc.cap_set_id) { found = 1; max_ver = ci->max_version; }
    }
    if (!found || gc.cap_set_ver > max_ver) return -EINVAL;
    static uint8_t buf[VIRTIO_GPU_CAPSET_BUF];
    int n = virtio_gpu_get_capset(gc.cap_set_id, gc.cap_set_ver, buf, sizeof(buf));
    dbg_num("DRM:   host capset bytes=", n);
    if (n < 0) return -EINVAL;
    uint32_t k = (uint32_t)n < gc.size ? (uint32_t)n : gc.size;
    if (copy_to_user((void *)gc.addr, buf, k)) return -EFAULT;
    return 0;
}

static int64_t ioctl_resource_create(uint32_t slot, DrmClient *c, uint64_t argp)
{
    struct virtgpu_resource_create_u rc;
    if (copy_from_user(&rc, (void *)argp, sizeof(rc))) return -EFAULT;
    if (!client_ensure_ctx(c)) return -ENODEV;

    uint32_t size = rc.size ? rc.size : PAGE_SZ;
    uint32_t pages = (size + PAGE_SZ - 1) / PAGE_SZ;

    uint32_t h;
    for (h = 0; h < DRM_MAX_BOS && g_bos[h].used; h++) {}
    if (h == DRM_MAX_BOS) return -ENOSPC;

    void *mem = pmm_alloc_contiguous(pages);
    if (!mem) return -ENOMEM;
    uint64_t phys = (uint64_t)(uintptr_t)mem;
    memset((void *)(uintptr_t)phys, 0, (size_t)pages * PAGE_SZ); /* identity-mapped RAM */

    DrmBo *bo = &g_bos[h];
    memset(bo, 0, sizeof(*bo));
    bo->used = true;
    bo->client = slot;
    bo->res_id = vgpu_alloc_resource_id();
    bo->phys = phys;
    bo->pages = pages;
    bo->size = pages * PAGE_SZ;

    VirtioGpuResourceCreate3d cr;
    memset(&cr, 0, sizeof(cr));
    cr.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    cr.resource_id = bo->res_id;
    cr.target = rc.target;
    cr.format = rc.format;
    cr.bind = rc.bind;
    cr.width = rc.width;
    cr.height = rc.height;
    cr.depth = rc.depth;
    cr.array_size = rc.array_size;
    cr.last_level = rc.last_level;
    cr.nr_samples = rc.nr_samples;
    cr.flags = rc.flags;
    uint32_t rt = 0;
    if (!send_sync(&cr, sizeof(cr), &rt) || rt != VIRTIO_GPU_RESP_OK_NODATA) {
        for (uint32_t i = 0; i < pages; i++) pmm_free_page((void *)(uintptr_t)(phys + (uint64_t)i * PAGE_SZ));
        memset(bo, 0, sizeof(*bo));
        return -EINVAL;
    }

    VirtioGpuAttachBacking1 ab;
    memset(&ab, 0, sizeof(ab));
    ab.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    ab.resource_id = bo->res_id;
    ab.nr_entries = 1;
    ab.entry.addr = phys;
    ab.entry.length = bo->size;
    VirtioGpuCtxResource at;
    memset(&at, 0, sizeof(at));
    at.hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    at.hdr.ctx_id = c->ctx_id;
    at.resource_id = bo->res_id;
    if (!send_sync(&ab, sizeof(ab), &rt) || rt != VIRTIO_GPU_RESP_OK_NODATA ||
        !send_sync(&at, sizeof(at), &rt) || rt != VIRTIO_GPU_RESP_OK_NODATA) {
        bo_destroy(c, bo);
        return -EINVAL;
    }

    rc.bo_handle = h + 1;
    rc.res_handle = bo->res_id;
    if (copy_to_user((void *)argp, &rc, sizeof(rc))) return -EFAULT;
    return 0;
}

static int64_t ioctl_resource_info(uint32_t slot, uint64_t argp)
{
    struct virtgpu_resource_info_u ri;
    if (copy_from_user(&ri, (void *)argp, sizeof(ri))) return -EFAULT;
    DrmBo *bo = bo_lookup(slot, ri.bo_handle);
    if (!bo) return -ENOENT;
    ri.res_handle = bo->res_id;
    ri.size = bo->size;
    ri.blob_mem = 0;
    if (copy_to_user((void *)argp, &ri, sizeof(ri))) return -EFAULT;
    return 0;
}

static int64_t ioctl_map(uint32_t slot, uint64_t argp)
{
    struct virtgpu_map_u m;
    if (copy_from_user(&m, (void *)argp, sizeof(m))) return -EFAULT;
    if (!bo_lookup(slot, m.handle)) return -ENOENT;
    m.offset = (uint64_t)m.handle << 32; /* page-aligned, unique per BO */
    if (copy_to_user((void *)argp, &m, sizeof(m))) return -EFAULT;
    return 0;
}

static int64_t ioctl_transfer(uint32_t slot, DrmClient *c, uint64_t argp, bool to_host)
{
    struct virtgpu_transfer_u tr;
    if (copy_from_user(&tr, (void *)argp, sizeof(tr))) return -EFAULT;
    DrmBo *bo = bo_lookup(slot, tr.bo_handle);
    if (!bo) return -ENOENT;
    if (!client_ensure_ctx(c)) return -ENODEV;

    uint32_t len = sizeof(VirtioGpuTransferHost3d);
    uint8_t *mem = (uint8_t *)kmalloc(len + sizeof(VirtioGpuCtrlResponse));
    if (!mem) return -ENOMEM;
    memset(mem, 0, len + sizeof(VirtioGpuCtrlResponse));
    VirtioGpuTransferHost3d *cmd = (VirtioGpuTransferHost3d *)mem;
    cmd->hdr.type = to_host ? VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D : VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    cmd->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    cmd->hdr.fence_id = vgpu_next_fence_id();
    cmd->hdr.ctx_id = c->ctx_id;
    cmd->box.x = tr.box.x; cmd->box.y = tr.box.y; cmd->box.z = tr.box.z;
    cmd->box.w = tr.box.w; cmd->box.h = tr.box.h; cmd->box.d = tr.box.d;
    cmd->offset = tr.offset;
    cmd->resource_id = bo->res_id;
    cmd->level = tr.level;
    cmd->stride = tr.stride;
    cmd->layer_stride = tr.layer_stride;

    VgpuTicket t;
    if (!submit_owned(mem, len, 0, 0, len, &t)) return -ENOMEM;
    bo->last = t;
    bo->has_last = true;
    return 0;
}

static int64_t ioctl_execbuffer(uint32_t slot, DrmClient *c, uint64_t argp, uint32_t argsz)
{
    struct virtgpu_execbuffer_u eb;
    memset(&eb, 0, sizeof(eb));
    if (argsz > sizeof(eb)) argsz = sizeof(eb);
    if (argsz < 32) return -EINVAL;
    if (copy_from_user(&eb, (void *)argp, argsz)) return -EFAULT;
    if (eb.flags) return -EINVAL; /* no fence fds / rings: we report DRM 0.0 */
    if (eb.size == 0) return 0;
    if (eb.size > DRM_MAX_CMDBUF || (eb.size & 3)) return -EINVAL;
    if (eb.num_bo_handles > DRM_MAX_BOS) return -EINVAL;
    if (!client_ensure_ctx(c)) return -ENODEV;

    /* Validate the BO list first so a bad handle fails the whole submit. */
    uint32_t *handles = NULL;
    if (eb.num_bo_handles) {
        handles = (uint32_t *)kmalloc((size_t)eb.num_bo_handles * 4);
        if (!handles) return -ENOMEM;
        if (copy_from_user(handles, (void *)eb.bo_handles, (uint64_t)eb.num_bo_handles * 4)) {
            kfree(handles);
            return -EFAULT;
        }
        for (uint32_t i = 0; i < eb.num_bo_handles; i++) {
            if (!bo_lookup(slot, handles[i])) { kfree(handles); return -ENOENT; }
        }
    }

    uint32_t hdr_len = sizeof(VirtioGpuCmdSubmit);
    uint32_t resp_off = (hdr_len + eb.size + 7) & ~7u;
    uint8_t *mem = (uint8_t *)kmalloc(resp_off + sizeof(VirtioGpuCtrlResponse));
    if (!mem) { if (handles) kfree(handles); return -ENOMEM; }
    memset(mem, 0, hdr_len);
    memset(mem + resp_off, 0, sizeof(VirtioGpuCtrlResponse));
    if (copy_from_user(mem + hdr_len, (void *)eb.command, eb.size)) {
        kfree(mem);
        if (handles) kfree(handles);
        return -EFAULT;
    }
    VirtioGpuCmdSubmit *cmd = (VirtioGpuCmdSubmit *)mem;
    cmd->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    cmd->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    cmd->hdr.fence_id = vgpu_next_fence_id();
    cmd->hdr.ctx_id = c->ctx_id;
    cmd->size = eb.size;

    VgpuTicket t;
    if (!submit_owned(mem, hdr_len, hdr_len, eb.size, resp_off, &t)) {
        if (handles) kfree(handles);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < eb.num_bo_handles; i++) {
        DrmBo *bo = bo_lookup(slot, handles[i]);
        bo->last = t;
        bo->has_last = true;
    }
    if (handles) kfree(handles);
    return 0;
}

static int64_t ioctl_wait(uint32_t slot, uint64_t argp)
{
    struct virtgpu_wait_u w;
    if (copy_from_user(&w, (void *)argp, sizeof(w))) return -EFAULT;
    DrmBo *bo = bo_lookup(slot, w.handle);
    if (!bo) return -ENOENT;
    if (!bo->has_last || vgpu_done(bo->last)) return 0;
    if (w.flags & VIRTGPU_WAIT_NOWAIT) return -EBUSY;
    return wait_ticket(bo->last, DRM_WAIT_MS) ? 0 : -ETIMEDOUT;
}

static int64_t ioctl_gem_close(uint32_t slot, DrmClient *c, uint64_t argp)
{
    struct drm_gem_close_u gc;
    if (copy_from_user(&gc, (void *)argp, sizeof(gc))) return -EFAULT;
    DrmBo *bo = bo_lookup(slot, gc.handle);
    if (!bo) return -EINVAL;
    bo_destroy(c, bo);
    return 0;
}

static int64_t ioctl_get_cap(uint64_t argp)
{
    struct drm_get_cap_u cap;
    if (copy_from_user(&cap, (void *)argp, sizeof(cap))) return -EFAULT;
    cap.value = 0; /* no PRIME, no syncobj, no dumb buffers yet */
    if (copy_to_user((void *)argp, &cap, sizeof(cap))) return -EFAULT;
    return 0;
}

static int64_t drm_ioctl_inner(uint32_t *slot, uint64_t request, uint64_t argp);

int64_t drm_ioctl(uint32_t *slot, uint64_t request, uint64_t argp)
{
    int64_t r = drm_ioctl_inner(slot, request, argp);
#if DRM_DEBUG
    uint32_t nr = request & 0xFF;
    /* EXECBUFFER/WAIT/TRANSFER are per-frame: only log them on error */
    bool hot = nr == DRM_COMMAND_BASE + VIRTGPU_EXECBUFFER || nr == DRM_COMMAND_BASE + VIRTGPU_WAIT ||
               nr == DRM_COMMAND_BASE + VIRTGPU_TRANSFER_TO_HOST || nr == DRM_COMMAND_BASE + VIRTGPU_TRANSFER_FROM_HOST;
    if (!hot || r != 0) {
        dbg_num("DRM: ioctl nr=", nr);
        dbg_num("DRM:   -> ", r);
    }
#endif
    return r;
}

static int64_t drm_ioctl_inner(uint32_t *slot, uint64_t request, uint64_t argp)
{
    if (((request >> 8) & 0xFF) != DRM_IOCTL_TYPE) return -ENOTTY;
    uint32_t nr = request & 0xFF;
    uint32_t argsz = (request >> 16) & 0x3FFF;

    pending_gc();

    switch (nr) {
    case DRM_NR_VERSION:        return ioctl_version(argp);
    case DRM_NR_GET_UNIQUE:     return 0;
    case DRM_NR_GET_CAP:        return ioctl_get_cap(argp);
    case DRM_NR_SET_CLIENT_CAP: return -EINVAL;
    default: break;
    }
    if (!g_virgl) return -ENODEV;

    DrmClient *c = client_get(slot);
    if (!c) return -ENOMEM;

    switch (nr) {
    case DRM_NR_GEM_CLOSE:                          return ioctl_gem_close(*slot, c, argp);
    case DRM_COMMAND_BASE + VIRTGPU_GETPARAM:        return ioctl_getparam(argp);
    case DRM_COMMAND_BASE + VIRTGPU_GET_CAPS:        return ioctl_get_caps(argp);
    case DRM_COMMAND_BASE + VIRTGPU_RESOURCE_CREATE: return ioctl_resource_create(*slot, c, argp);
    case DRM_COMMAND_BASE + VIRTGPU_RESOURCE_INFO:   return ioctl_resource_info(*slot, argp);
    case DRM_COMMAND_BASE + VIRTGPU_MAP:             return ioctl_map(*slot, argp);
    case DRM_COMMAND_BASE + VIRTGPU_EXECBUFFER:      return ioctl_execbuffer(*slot, c, argp, argsz);
    case DRM_COMMAND_BASE + VIRTGPU_TRANSFER_TO_HOST:   return ioctl_transfer(*slot, c, argp, true);
    case DRM_COMMAND_BASE + VIRTGPU_TRANSFER_FROM_HOST: return ioctl_transfer(*slot, c, argp, false);
    case DRM_COMMAND_BASE + VIRTGPU_WAIT:            return ioctl_wait(*slot, argp);
    default:
        return -EINVAL; /* blob resources, CONTEXT_INIT, PRIME: not yet */
    }
}

int drm_mmap_lookup(uint32_t slot, uint64_t offset, uint64_t len, uint64_t *phys)
{
    uint32_t handle = (uint32_t)(offset >> 32);
    DrmBo *bo = bo_lookup(slot, handle);
    if (!bo || (offset & 0xFFFFFFFFull) != 0 || len > bo->size) return -EINVAL;
    *phys = bo->phys;
    return 0;
}

/* Destroy every BO and the host context of a client. Sleeps (waits for
   the host): syscall context only. */
static void client_free(uint32_t slot)
{
    DrmClient *c = &g_clients[slot - 1];
    c->orphaned = false; /* bo_destroy() -> pending_gc() must not recurse here */
    for (uint32_t i = 0; i < DRM_MAX_BOS; i++) {
        if (g_bos[i].used && g_bos[i].client == slot) bo_destroy(c, &g_bos[i]);
    }
    if (c->ctx_id) {
        VirtioGpuCtrlHeader d;
        memset(&d, 0, sizeof(d));
        d.type = VIRTIO_GPU_CMD_CTX_DESTROY;
        d.ctx_id = c->ctx_id;
        send_sync(&d, sizeof(d), NULL);
    }
    memset(c, 0, sizeof(*c));
}

void drm_release(uint32_t slot, uint64_t pid, bool can_sleep)
{
    if (slot == 0 || slot > DRM_MAX_CLIENTS) return;
    DrmClient *c = &g_clients[slot - 1];
    if (!c->used || c->pid != pid) return;
    if (can_sleep) client_free(slot);
    else c->orphaned = true; /* process teardown: can't wait for the host there */
}
