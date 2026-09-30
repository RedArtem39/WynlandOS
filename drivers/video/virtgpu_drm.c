/*
 * WynlandOS - DRM render node for virtio-gpu 3D (virgl)
 * ============================================================
 * Implements the subset of the Linux virtio_gpu DRM uAPI that Mesa's virgl
 * winsys (src/gallium/winsys/virgl/drm/virgl_drm_winsys.c) uses:
 *
 *   GETPARAM, GET_CAPS, RESOURCE_CREATE, RESOURCE_INFO, MAP (+ mmap),
 *   EXECBUFFER, TRANSFER_TO_HOST, TRANSFER_FROM_HOST, WAIT, GEM_CLOSE
 *   plus the generic DRM_IOCTL_VERSION / GET_CAP,
 * and the pieces of the generic DRM/KMS uAPI a GPU compositor needs:
 *   PRIME (dma-buf fds: HANDLE_TO_FD / FD_TO_HANDLE, mmap), dumb buffers,
 *   and legacy modesetting on one virtual connector/encoder/CRTC --
 *   GETRESOURCES / GETCONNECTOR / GETENCODER / GETCRTC / SETCRTC, ADDFB,
 *   ADDFB2, RMFB, DIRTYFB, PAGE_FLIP with flip-complete events read()
 *   from the DRM fd. A framebuffer goes on screen by SET_SCANOUT of its
 *   own host resource: a GPU-rendered frame is displayed without ever
 *   being copied through the CPU.
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
#include <wynland/waitqueue.h>

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
#define EAGAIN  11
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
#define DRM_NR_SET_MASTER       0x1e
#define DRM_NR_DROP_MASTER      0x1f
#define DRM_NR_PRIME_HANDLE_TO_FD 0x2d
#define DRM_NR_PRIME_FD_TO_HANDLE 0x2e
#define DRM_NR_MODE_GETRESOURCES  0xA0
#define DRM_NR_MODE_GETCRTC       0xA1
#define DRM_NR_MODE_SETCRTC       0xA2
#define DRM_NR_MODE_CURSOR        0xA3
#define DRM_NR_MODE_GETENCODER    0xA6
#define DRM_NR_MODE_GETCONNECTOR  0xA7
#define DRM_NR_MODE_GETPROPERTY   0xAA
#define DRM_NR_MODE_ADDFB         0xAE
#define DRM_NR_MODE_RMFB          0xAF
#define DRM_NR_MODE_PAGE_FLIP     0xB0
#define DRM_NR_MODE_DIRTYFB       0xB1
#define DRM_NR_MODE_CREATE_DUMB   0xB2
#define DRM_NR_MODE_MAP_DUMB      0xB3
#define DRM_NR_MODE_DESTROY_DUMB  0xB4
#define DRM_NR_MODE_GETPLANERESOURCES 0xB5
#define DRM_NR_MODE_ADDFB2        0xB8
#define DRM_NR_MODE_OBJ_GETPROPERTIES 0xB9

/* KMS object ids (one of each; ids are unique across object types) */
#define KMS_CONNECTOR_ID 10
#define KMS_ENCODER_ID   20
#define KMS_CRTC_ID      30

#define DRM_FORMAT_XRGB8888 0x34325258 /* 'XR24' */
#define DRM_FORMAT_ARGB8888 0x34325241 /* 'AR24' */
#define DRM_FORMAT_XBGR8888 0x34324258 /* 'XB24' */
#define DRM_FORMAT_ABGR8888 0x34324241 /* 'AB24' */

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
struct drm_set_client_cap_u { uint64_t capability; uint64_t value; };
struct drm_prime_handle_u { uint32_t handle; uint32_t flags; int32_t fd; };

struct drm_mode_modeinfo_u {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char     name[32];
};
struct drm_mode_card_res_u {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};
struct drm_mode_crtc_u {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct drm_mode_modeinfo_u mode;
};
struct drm_mode_get_encoder_u { uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones; };
struct drm_mode_get_connector_u {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id, connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height, subpixel, pad;
};
struct drm_mode_fb_cmd_u  { uint32_t fb_id, width, height, pitch, bpp, depth, handle; };
struct drm_mode_fb_cmd2_u {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifier[4];
};
struct drm_mode_crtc_page_flip_u { uint32_t crtc_id, fb_id, flags, reserved; uint64_t user_data; };
struct drm_mode_fb_dirty_cmd_u   { uint32_t fb_id, flags, color, num_clips; uint64_t clips_ptr; };
struct drm_mode_create_dumb_u    { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct drm_mode_map_dumb_u       { uint32_t handle, pad; uint64_t offset; };
struct drm_mode_get_plane_res_u  { uint64_t plane_id_ptr; uint32_t count_planes; };
struct drm_mode_obj_get_properties_u { uint64_t props_ptr, prop_values_ptr; uint32_t count_props, obj_id, obj_type; };
struct drm_event_vblank_u {
    uint32_t type, length;
    uint64_t user_data;
    uint32_t tv_sec, tv_usec, sequence, crtc_id;
};
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

#define DRM_MAX_EVENTS 8

typedef struct {
    bool       used;
    uint64_t   user_data;
    VgpuTicket t;        /* the flip's RESOURCE_FLUSH: delivered once done */
} DrmEvent;

typedef struct {
    bool     used;
    uint64_t pid;       /* opener */
    uint32_t ctx_id;    /* 0 until the first 3D call */
    bool     orphaned;  /* opener exited; free from the next ioctl */
    DrmEvent events[DRM_MAX_EVENTS]; /* pending page-flip events, FIFO by seq */
    uint32_t ev_seq;
    WaitQueue ev_wq;    /* read()/poll() sleepers; woken from drm_tick() */
} DrmClient;

/* A buffer object: one host resource + its guest backing. Shared by every
   client that holds a handle to it (bit (slot-1) in `clients`: the
   creator, plus importers via PRIME) and kept alive by `refs` (dma-buf
   fds, framebuffers). Destroyed -- from syscall context, since that waits
   for the host -- once both drop to zero. */
typedef struct {
    bool       used;
    uint32_t   clients;
    uint32_t   refs;
    bool       zombie;  /* clients == refs == 0: destroy from pending_gc() */
    bool       is_3d;   /* virgl resource (vs a 2D dumb buffer) */
    uint32_t   width, height, stride; /* dumb buffers */
    uint32_t   res_id;
    uint64_t   phys;
    uint32_t   pages;
    uint32_t   size;
    bool       has_last;
    VgpuTicket last;    /* last host request touching this BO */
} DrmBo;

#define DRM_MAX_FBS 64

typedef struct {
    bool     used;
    uint32_t id;
    uint32_t bo;        /* BO index + 1 */
    uint32_t owner;     /* client slot */
    uint32_t width, height, pitch, format;
} DrmFb;

typedef struct {
    void      *mem;
    VgpuTicket t;
} DrmPending;

static DrmClient  g_clients[DRM_MAX_CLIENTS];
static DrmBo      g_bos[DRM_MAX_BOS];       /* GEM handle = index + 1 */
static DrmPending g_pending[DRM_MAX_PENDING];
static DrmFb      g_fbs[DRM_MAX_FBS];
static uint32_t   g_next_fb_id = 100;
static uint32_t   g_scan_fb;    /* fb on the CRTC; 0 = the kernel console (resource 1) */
static uint32_t   g_scan_owner; /* client slot that put it there */

/* ---- helpers ---- */

static void client_free(uint32_t slot);
static void bo_destroy(DrmBo *bo);
static void fb_remove(DrmFb *fb);

static void pending_gc(void)
{
    for (uint32_t i = 0; i < DRM_MAX_CLIENTS; i++) {
        if (g_clients[i].used && g_clients[i].orphaned) client_free(i + 1);
    }
    for (uint32_t i = 0; i < DRM_MAX_BOS; i++) {
        if (g_bos[i].used && g_bos[i].zombie) bo_destroy(&g_bos[i]);
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
    if (handle == 0 || handle > DRM_MAX_BOS || client_slot == 0) return NULL;
    DrmBo *bo = &g_bos[handle - 1];
    if (!bo->used || bo->zombie || !(bo->clients & (1u << (client_slot - 1)))) return NULL;
    return bo;
}

/* No holder left: destroy it later from pending_gc() (process teardown
   and dma-buf close can't wait for the host). */
static void bo_put(DrmBo *bo)
{
    if (bo->used && bo->clients == 0 && bo->refs == 0) bo->zombie = true;
}

/* Client `slot` drops its handle: detach from its virgl context. */
static void bo_release_client(uint32_t slot, DrmClient *c, DrmBo *bo)
{
    if (bo->is_3d && c && c->ctx_id) {
        VirtioGpuCtxResource cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.hdr.type = VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE;
        cmd.hdr.ctx_id = c->ctx_id;
        cmd.resource_id = bo->res_id;
        send_sync(&cmd, sizeof(cmd), NULL);
    }
    bo->clients &= ~(1u << (slot - 1));
    bo_put(bo);
}

/* Final destruction: host resource and guest pages. Sleeps. */
static void bo_destroy(DrmBo *bo)
{
    if (bo->has_last) wait_ticket(bo->last, DRM_WAIT_MS);

    VirtioGpuCtxResource cmd;
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
    bo->clients = 1u << (slot - 1);
    bo->is_3d = true;
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
        bo_destroy(bo);
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
    bo_release_client(slot, c, bo);
    return 0;
}

static int64_t ioctl_get_cap(uint64_t argp)
{
    struct drm_get_cap_u cap;
    if (copy_from_user(&cap, (void *)argp, sizeof(cap))) return -EFAULT;
    switch (cap.capability) {
    case 0x1:  cap.value = 1;  break;  /* DRM_CAP_DUMB_BUFFER */
    case 0x5:  cap.value = 3;  break;  /* DRM_CAP_PRIME: import | export */
    case 0x6:  cap.value = 1;  break;  /* DRM_CAP_TIMESTAMP_MONOTONIC */
    case 0x8:  cap.value = 64; break;  /* DRM_CAP_CURSOR_WIDTH */
    case 0x9:  cap.value = 64; break;  /* DRM_CAP_CURSOR_HEIGHT */
    case 0x12: cap.value = 1;  break;  /* DRM_CAP_CRTC_IN_VBLANK_EVENT */
    default:   cap.value = 0;  break;  /* no syncobj, async flip, modifiers */
    }
    if (copy_to_user((void *)argp, &cap, sizeof(cap))) return -EFAULT;
    return 0;
}

/* ============================================================
 * PRIME (dma-buf)
 * ============================================================ */

static int64_t ioctl_prime_handle_to_fd(uint32_t slot, uint64_t argp)
{
    struct drm_prime_handle_u ph;
    if (copy_from_user(&ph, (void *)argp, sizeof(ph))) return -EFAULT;
    DrmBo *bo = bo_lookup(slot, ph.handle);
    if (!bo) return -ENOENT;
    int fd = drm_install_prime_fd((uint32_t)(bo - g_bos) + 1, (ph.flags & 02000000) != 0 /* O_CLOEXEC */);
    if (fd < 0) return fd;
    bo->refs++;
    ph.fd = fd;
    if (copy_to_user((void *)argp, &ph, sizeof(ph))) return -EFAULT;
    return 0;
}

static int64_t ioctl_prime_fd_to_handle(uint32_t slot, DrmClient *c, uint64_t argp)
{
    struct drm_prime_handle_u ph;
    if (copy_from_user(&ph, (void *)argp, sizeof(ph))) return -EFAULT;
    uint32_t idx = drm_prime_fd_bo(ph.fd);
    if (idx == 0 || idx > DRM_MAX_BOS || !g_bos[idx - 1].used || g_bos[idx - 1].zombie) return -EINVAL;
    DrmBo *bo = &g_bos[idx - 1];
    uint32_t bit = 1u << (slot - 1);
    if (!(bo->clients & bit)) {
        /* the importer's virgl context must know the resource too */
        if (bo->is_3d) {
            if (!client_ensure_ctx(c)) return -ENODEV;
            VirtioGpuCtxResource at;
            memset(&at, 0, sizeof(at));
            at.hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
            at.hdr.ctx_id = c->ctx_id;
            at.resource_id = bo->res_id;
            uint32_t rt = 0;
            if (!send_sync(&at, sizeof(at), &rt) || rt != VIRTIO_GPU_RESP_OK_NODATA) return -EINVAL;
        }
        bo->clients |= bit;
    }
    ph.handle = idx;
    if (copy_to_user((void *)argp, &ph, sizeof(ph))) return -EFAULT;
    return 0;
}

void drm_prime_get(uint32_t idx)
{
    if (idx && idx <= DRM_MAX_BOS && g_bos[idx - 1].used) g_bos[idx - 1].refs++;
}

void drm_prime_put(uint32_t idx)
{
    if (!idx || idx > DRM_MAX_BOS || !g_bos[idx - 1].used) return;
    DrmBo *bo = &g_bos[idx - 1];
    if (bo->refs) bo->refs--;
    bo_put(bo);
}

int drm_prime_mmap_lookup(uint32_t idx, uint64_t offset, uint64_t len, uint64_t *phys)
{
    if (!idx || idx > DRM_MAX_BOS || !g_bos[idx - 1].used || g_bos[idx - 1].zombie) return -EINVAL;
    DrmBo *bo = &g_bos[idx - 1];
    if (offset & (PAGE_SZ - 1) || offset + len > bo->size) return -EINVAL;
    *phys = bo->phys + offset;
    return 0;
}

/* ============================================================
 * Dumb buffers (2D, CPU-drawn)
 * ============================================================ */

static int64_t ioctl_create_dumb(uint32_t slot, uint64_t argp)
{
    struct drm_mode_create_dumb_u cd;
    if (copy_from_user(&cd, (void *)argp, sizeof(cd))) return -EFAULT;
    if (cd.bpp != 32 || cd.width == 0 || cd.height == 0 || cd.width > 8192 || cd.height > 8192) return -EINVAL;
    uint32_t pitch = cd.width * 4;
    uint32_t size = pitch * cd.height;
    uint32_t pages = (size + PAGE_SZ - 1) / PAGE_SZ;

    uint32_t h;
    for (h = 0; h < DRM_MAX_BOS && g_bos[h].used; h++) {}
    if (h == DRM_MAX_BOS) return -ENOSPC;
    void *mem = pmm_alloc_contiguous(pages);
    if (!mem) return -ENOMEM;
    uint64_t phys = (uint64_t)(uintptr_t)mem;
    memset((void *)(uintptr_t)phys, 0, (size_t)pages * PAGE_SZ);

    DrmBo *bo = &g_bos[h];
    memset(bo, 0, sizeof(*bo));
    bo->used = true;
    bo->clients = 1u << (slot - 1);
    bo->res_id = vgpu_alloc_resource_id();
    bo->phys = phys;
    bo->pages = pages;
    bo->size = pages * PAGE_SZ;
    bo->width = cd.width;
    bo->height = cd.height;
    bo->stride = pitch;

    VirtioGpuResourceCreate2d cr;
    memset(&cr, 0, sizeof(cr));
    cr.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cr.resource_id = bo->res_id;
    cr.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM; /* = DRM XRGB8888 in memory */
    cr.width = cd.width;
    cr.height = cd.height;
    VirtioGpuAttachBacking1 ab;
    memset(&ab, 0, sizeof(ab));
    ab.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    ab.resource_id = bo->res_id;
    ab.nr_entries = 1;
    ab.entry.addr = phys;
    ab.entry.length = bo->size;
    uint32_t rt = 0;
    if (!send_sync(&cr, sizeof(cr), &rt) || rt != VIRTIO_GPU_RESP_OK_NODATA ||
        !send_sync(&ab, sizeof(ab), &rt) || rt != VIRTIO_GPU_RESP_OK_NODATA) {
        bo->clients = 0;
        bo_destroy(bo);
        return -EINVAL;
    }
    cd.handle = h + 1;
    cd.pitch = pitch;
    cd.size = size;
    if (copy_to_user((void *)argp, &cd, sizeof(cd))) return -EFAULT;
    return 0;
}

static int64_t ioctl_map_dumb(uint32_t slot, uint64_t argp)
{
    struct drm_mode_map_dumb_u md;
    if (copy_from_user(&md, (void *)argp, sizeof(md))) return -EFAULT;
    if (!bo_lookup(slot, md.handle)) return -ENOENT;
    md.offset = (uint64_t)md.handle << 32; /* same scheme as VIRTGPU_MAP */
    if (copy_to_user((void *)argp, &md, sizeof(md))) return -EFAULT;
    return 0;
}

/* ============================================================
 * KMS: one connector -> encoder -> CRTC, scanout 0
 * ============================================================ */

extern uint32_t comp_get_width(void);
extern uint32_t comp_get_height(void);

static void kms_mode(struct drm_mode_modeinfo_u *m)
{
    uint32_t w = comp_get_width(), h = comp_get_height();
    memset(m, 0, sizeof(*m));
    m->hdisplay = (uint16_t)w; m->hsync_start = (uint16_t)(w + 16);
    m->hsync_end = (uint16_t)(w + 32); m->htotal = (uint16_t)(w + 48);
    m->vdisplay = (uint16_t)h; m->vsync_start = (uint16_t)(h + 3);
    m->vsync_end = (uint16_t)(h + 6); m->vtotal = (uint16_t)(h + 10);
    m->vrefresh = 60;
    m->clock = (uint32_t)((uint64_t)m->htotal * m->vtotal * 60 / 1000);
    m->type = 0x48; /* DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER */
    /* name "WxH" */
    char tmp[12]; int n = 0, k = 0;
    uint32_t v[2] = { w, h };
    for (int i = 0; i < 2; i++) {
        uint32_t x = v[i]; n = 0;
        do { tmp[n++] = (char)('0' + x % 10); x /= 10; } while (x && n < 11);
        while (n && k < 30) m->name[k++] = tmp[--n];
        if (i == 0) m->name[k++] = 'x';
    }
}

static DrmFb *fb_find(uint32_t id)
{
    for (uint32_t i = 0; i < DRM_MAX_FBS; i++)
        if (g_fbs[i].used && g_fbs[i].id == id) return &g_fbs[i];
    return NULL;
}

/* Put the kernel's own framebuffer (resource 1) back on screen. */
static void kms_restore_console(void)
{
    dbg_num("DRM: console restored at ms=", (int64_t)timer_get_ms());
    VirtioGpuSetScanout ss;
    memset(&ss, 0, sizeof(ss));
    ss.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    ss.rww = comp_get_width();
    ss.rhh = comp_get_height();
    ss.scanout_id = 0;
    ss.resource_id = 1;
    send_sync(&ss, sizeof(ss), NULL);
    g_scan_fb = 0;
    g_scan_owner = 0;
    extern void virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    virtio_gpu_flush(0, 0, comp_get_width(), comp_get_height());
}

static void fb_remove(DrmFb *fb)
{
    if (g_scan_fb == fb->id) kms_restore_console();
    DrmBo *bo = &g_bos[fb->bo - 1];
    if (bo->refs) bo->refs--;
    bo_put(bo);
    memset(fb, 0, sizeof(*fb));
}

/* Queue an async command whose buffer is freed once it completes. */
static bool send_async(const void *cmd, uint32_t len, VgpuTicket *t)
{
    uint8_t *mem = (uint8_t *)kmalloc(len + sizeof(VirtioGpuCtrlResponse));
    if (!mem) return false;
    memcpy(mem, cmd, len);
    memset(mem + len, 0, sizeof(VirtioGpuCtrlResponse));
    return submit_owned(mem, len, 0, 0, len, t);
}

/* Show `fb` on scanout 0 (if not already) and push it to the display.
   Returns the RESOURCE_FLUSH ticket: flip completion. */
static bool kms_present(DrmFb *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, VgpuTicket *done)
{
    DrmBo *bo = &g_bos[fb->bo - 1];
    /* Never scan out a frame the GPU is still drawing: wait for the last
       submission that touched this buffer. */
    if (bo->has_last) wait_ticket(bo->last, DRM_WAIT_MS);

    if (!bo->is_3d) {
        VirtioGpuTransferToHost2d tr;
        memset(&tr, 0, sizeof(tr));
        tr.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
        tr.resource_id = bo->res_id;
        tr.rxx = x; tr.ryy = y; tr.rww = w; tr.rhh = h;
        tr.offset = (uint64_t)y * fb->pitch + (uint64_t)x * 4;
        VgpuTicket t;
        if (!send_async(&tr, sizeof(tr), &t)) return false;
    }
    if (g_scan_fb != fb->id) {
        VirtioGpuSetScanout ss;
        memset(&ss, 0, sizeof(ss));
        ss.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
        ss.rww = fb->width;
        ss.rhh = fb->height;
        ss.scanout_id = 0;
        ss.resource_id = bo->res_id;
        uint32_t rt = 0;
        bool ok = send_sync(&ss, sizeof(ss), &rt);
        if (!ok || rt != VIRTIO_GPU_RESP_OK_NODATA) dbg_num("DRM: SET_SCANOUT failed, resp=", ok ? rt : -1);
        if (!ok || rt != VIRTIO_GPU_RESP_OK_NODATA) return false;
        g_scan_fb = fb->id;
    }
    VirtioGpuResourceFlush rf;
    memset(&rf, 0, sizeof(rf));
    rf.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    rf.resource_id = bo->res_id;
    rf.rxx = x; rf.ryy = y; rf.rww = w; rf.rhh = h;
    return send_async(&rf, sizeof(rf), done);
}

static int64_t ioctl_getresources(uint64_t argp)
{
    struct drm_mode_card_res_u r;
    if (copy_from_user(&r, (void *)argp, sizeof(r))) return -EFAULT;
    uint32_t one[1];
    if (r.count_crtcs >= 1 && r.crtc_id_ptr) { one[0] = KMS_CRTC_ID; if (copy_to_user((void *)r.crtc_id_ptr, one, 4)) return -EFAULT; }
    if (r.count_connectors >= 1 && r.connector_id_ptr) { one[0] = KMS_CONNECTOR_ID; if (copy_to_user((void *)r.connector_id_ptr, one, 4)) return -EFAULT; }
    if (r.count_encoders >= 1 && r.encoder_id_ptr) { one[0] = KMS_ENCODER_ID; if (copy_to_user((void *)r.encoder_id_ptr, one, 4)) return -EFAULT; }
    r.count_fbs = 0;
    r.count_crtcs = r.count_connectors = r.count_encoders = 1;
    r.min_width = r.min_height = 1;
    r.max_width = r.max_height = 8192;
    if (copy_to_user((void *)argp, &r, sizeof(r))) return -EFAULT;
    return 0;
}

static int64_t ioctl_getconnector(uint64_t argp)
{
    struct drm_mode_get_connector_u c;
    if (copy_from_user(&c, (void *)argp, sizeof(c))) return -EFAULT;
    if (c.connector_id != KMS_CONNECTOR_ID) return -ENOENT;
    if (c.count_modes >= 1 && c.modes_ptr) {
        struct drm_mode_modeinfo_u m;
        kms_mode(&m);
        if (copy_to_user((void *)c.modes_ptr, &m, sizeof(m))) return -EFAULT;
    }
    if (c.count_encoders >= 1 && c.encoders_ptr) {
        uint32_t e = KMS_ENCODER_ID;
        if (copy_to_user((void *)c.encoders_ptr, &e, 4)) return -EFAULT;
    }
    c.count_modes = 1;
    c.count_props = 0;
    c.count_encoders = 1;
    c.encoder_id = KMS_ENCODER_ID;
    c.connector_type = 15;       /* DRM_MODE_CONNECTOR_VIRTUAL */
    c.connector_type_id = 1;
    c.connection = 1;            /* connected */
    c.mm_width = comp_get_width() * 254 / 960;   /* ~96 dpi */
    c.mm_height = comp_get_height() * 254 / 960;
    c.subpixel = 1;              /* unknown */
    if (copy_to_user((void *)argp, &c, sizeof(c))) return -EFAULT;
    return 0;
}

static int64_t ioctl_getencoder(uint64_t argp)
{
    struct drm_mode_get_encoder_u e;
    if (copy_from_user(&e, (void *)argp, sizeof(e))) return -EFAULT;
    if (e.encoder_id != KMS_ENCODER_ID) return -ENOENT;
    e.encoder_type = 5;          /* DRM_MODE_ENCODER_VIRTUAL */
    e.crtc_id = KMS_CRTC_ID;
    e.possible_crtcs = 1;
    e.possible_clones = 0;
    if (copy_to_user((void *)argp, &e, sizeof(e))) return -EFAULT;
    return 0;
}

static int64_t ioctl_getcrtc(uint64_t argp)
{
    struct drm_mode_crtc_u c;
    if (copy_from_user(&c, (void *)argp, sizeof(c))) return -EFAULT;
    if (c.crtc_id != KMS_CRTC_ID) return -ENOENT;
    c.fb_id = g_scan_fb;
    c.x = c.y = 0;
    c.gamma_size = 0;
    c.mode_valid = 1;
    kms_mode(&c.mode);
    if (copy_to_user((void *)argp, &c, sizeof(c))) return -EFAULT;
    return 0;
}

static int64_t ioctl_setcrtc(uint32_t slot, uint64_t argp)
{
    struct drm_mode_crtc_u c;
    if (copy_from_user(&c, (void *)argp, sizeof(c))) return -EFAULT;
    if (c.crtc_id != KMS_CRTC_ID) return -ENOENT;
    if (c.fb_id == 0) {                     /* disable: the console comes back */
        if (g_scan_fb) kms_restore_console();
        return 0;
    }
    DrmFb *fb = fb_find(c.fb_id);
    if (!fb || fb->owner != slot) return -ENOENT;
    VgpuTicket t;
    if (!kms_present(fb, 0, 0, fb->width, fb->height, &t)) return -ENOMEM;
    g_scan_owner = slot;
    return 0;
}

static int64_t fb_create(uint32_t slot, uint32_t handle, uint32_t w, uint32_t h,
                         uint32_t pitch, uint32_t format, uint32_t *out_id)
{
    DrmBo *bo = bo_lookup(slot, handle);
    if (!bo) return -ENOENT;
    if (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888 &&
        format != DRM_FORMAT_XBGR8888 && format != DRM_FORMAT_ABGR8888) return -EINVAL;
    if (w == 0 || h == 0 || (uint64_t)pitch * h > bo->size) return -EINVAL;
    for (uint32_t i = 0; i < DRM_MAX_FBS; i++) {
        if (g_fbs[i].used) continue;
        DrmFb *fb = &g_fbs[i];
        fb->used = true;
        fb->id = g_next_fb_id++;
        fb->bo = (uint32_t)(bo - g_bos) + 1;
        fb->owner = slot;
        fb->width = w; fb->height = h; fb->pitch = pitch; fb->format = format;
        bo->refs++;
        *out_id = fb->id;
        return 0;
    }
    return -ENOSPC;
}

static int64_t ioctl_addfb(uint32_t slot, uint64_t argp)
{
    struct drm_mode_fb_cmd_u f;
    if (copy_from_user(&f, (void *)argp, sizeof(f))) return -EFAULT;
    if (f.bpp != 32) return -EINVAL;
    uint32_t fmt = f.depth == 32 ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
    int64_t r = fb_create(slot, f.handle, f.width, f.height, f.pitch, fmt, &f.fb_id);
    if (r) return r;
    if (copy_to_user((void *)argp, &f, sizeof(f))) return -EFAULT;
    return 0;
}

static int64_t ioctl_addfb2(uint32_t slot, uint64_t argp)
{
    struct drm_mode_fb_cmd2_u f;
    if (copy_from_user(&f, (void *)argp, sizeof(f))) return -EFAULT;
    if (f.offsets[0] != 0) return -EINVAL;
    int64_t r = fb_create(slot, f.handles[0], f.width, f.height, f.pitches[0], f.pixel_format, &f.fb_id);
    if (r) return r;
    if (copy_to_user((void *)argp, &f, sizeof(f))) return -EFAULT;
    return 0;
}

static int64_t ioctl_rmfb(uint32_t slot, uint64_t argp)
{
    uint32_t id;
    if (copy_from_user(&id, (void *)argp, sizeof(id))) return -EFAULT;
    DrmFb *fb = fb_find(id);
    if (!fb || fb->owner != slot) return -ENOENT;
    fb_remove(fb);
    return 0;
}

static int64_t ioctl_dirtyfb(uint32_t slot, uint64_t argp)
{
    struct drm_mode_fb_dirty_cmd_u d;
    if (copy_from_user(&d, (void *)argp, sizeof(d))) return -EFAULT;
    DrmFb *fb = fb_find(d.fb_id);
    if (!fb || fb->owner != slot) return -ENOENT;
    if (g_scan_fb != fb->id) return 0; /* not on screen: nothing to push */
    VgpuTicket t;
    return kms_present(fb, 0, 0, fb->width, fb->height, &t) ? 0 : -ENOMEM;
}

static int64_t ioctl_page_flip(uint32_t slot, DrmClient *c, uint64_t argp)
{
    struct drm_mode_crtc_page_flip_u p;
    if (copy_from_user(&p, (void *)argp, sizeof(p))) return -EFAULT;
    if (p.crtc_id != KMS_CRTC_ID) return -ENOENT;
    DrmFb *fb = fb_find(p.fb_id);
    if (!fb || fb->owner != slot) return -ENOENT;
    DrmEvent *ev = NULL;
    if (p.flags & 0x01) { /* DRM_MODE_PAGE_FLIP_EVENT */
        for (uint32_t i = 0; i < DRM_MAX_EVENTS && !ev; i++)
            if (!c->events[i].used) ev = &c->events[i];
        if (!ev) return -EBUSY; /* a flip is already pending per Linux rules */
    }
    VgpuTicket t;
    if (!kms_present(fb, 0, 0, fb->width, fb->height, &t)) return -ENOMEM;
    g_scan_owner = slot;
    if (ev) {
        ev->used = true;
        ev->user_data = p.user_data;
        ev->t = t;
    }
    return 0;
}

/* ============================================================
 * Events: read() / poll() on the DRM fd
 * ============================================================ */

static bool event_ready(DrmClient *c)
{
    for (uint32_t i = 0; i < DRM_MAX_EVENTS; i++)
        if (c->events[i].used && vgpu_done(c->events[i].t)) return true;
    return false;
}

bool drm_poll_ready(uint32_t slot)
{
    if (slot == 0 || slot > DRM_MAX_CLIENTS || !g_clients[slot - 1].used) return false;
    return event_ready(&g_clients[slot - 1]);
}

WaitQueue *drm_event_wq(uint32_t slot)
{
    if (slot == 0 || slot > DRM_MAX_CLIENTS || !g_clients[slot - 1].used) return NULL;
    return &g_clients[slot - 1].ev_wq;
}

int64_t drm_read(uint32_t slot, uint64_t ubuf, uint64_t len, bool nonblock)
{
    if (slot == 0 || slot > DRM_MAX_CLIENTS || !g_clients[slot - 1].used) return -EAGAIN;
    DrmClient *c = &g_clients[slot - 1];
    if (len < sizeof(struct drm_event_vblank_u)) return -EINVAL;
    while (!event_ready(c)) {
        bool any = false;
        for (uint32_t i = 0; i < DRM_MAX_EVENTS; i++) any |= c->events[i].used;
        if (nonblock || !any) return -EAGAIN;
        waitqueue_wait_ms(&c->ev_wq, timer_get_ms() + 5);
    }
    uint64_t done = 0;
    for (uint32_t i = 0; i < DRM_MAX_EVENTS && done + sizeof(struct drm_event_vblank_u) <= len; i++) {
        DrmEvent *e = &c->events[i];
        if (!e->used || !vgpu_done(e->t)) continue;
        uint64_t ms = timer_get_ms();
        struct drm_event_vblank_u v;
        v.type = 0x02;                 /* DRM_EVENT_FLIP_COMPLETE */
        v.length = sizeof(v);
        v.user_data = e->user_data;
        v.tv_sec = (uint32_t)(ms / 1000);
        v.tv_usec = (uint32_t)((ms % 1000) * 1000);
        v.sequence = ++c->ev_seq;
        v.crtc_id = KMS_CRTC_ID;
        if (copy_to_user((void *)(ubuf + done), &v, sizeof(v))) return done ? (int64_t)done : -EFAULT;
        done += sizeof(v);
        e->used = false;
    }
    return (int64_t)done;
}

/* Timer hook (IRQ context, no sleeping): wake event readers whose flip
   finished. */
void drm_tick(void)
{
    for (uint32_t s = 0; s < DRM_MAX_CLIENTS; s++) {
        DrmClient *c = &g_clients[s];
        if (!c->used) continue;
        for (uint32_t i = 0; i < DRM_MAX_EVENTS; i++) {
            if (c->events[i].used && vgpu_done(c->events[i].t)) {
                waitqueue_wake_all(&c->ev_wq);
                break;
            }
        }
    }
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
    case DRM_NR_SET_CLIENT_CAP: {
        struct drm_set_client_cap_u cc;
        if (copy_from_user(&cc, (void *)argp, sizeof(cc))) return -EFAULT;
        /* no universal planes / atomic yet: clients fall back to legacy KMS */
        return cc.capability == 4 /* ASPECT_RATIO */ ? 0 : -EINVAL;
    }
    case DRM_NR_SET_MASTER:
    case DRM_NR_DROP_MASTER:    return 0;
    case DRM_NR_MODE_GETRESOURCES: return ioctl_getresources(argp);
    case DRM_NR_MODE_GETCONNECTOR: return ioctl_getconnector(argp);
    case DRM_NR_MODE_GETENCODER:   return ioctl_getencoder(argp);
    case DRM_NR_MODE_GETCRTC:      return ioctl_getcrtc(argp);
    case DRM_NR_MODE_GETPLANERESOURCES: {
        struct drm_mode_get_plane_res_u pr;
        if (copy_from_user(&pr, (void *)argp, sizeof(pr))) return -EFAULT;
        pr.count_planes = 0;
        return copy_to_user((void *)argp, &pr, sizeof(pr)) ? -EFAULT : 0;
    }
    case DRM_NR_MODE_OBJ_GETPROPERTIES: {
        struct drm_mode_obj_get_properties_u op;
        if (copy_from_user(&op, (void *)argp, sizeof(op))) return -EFAULT;
        op.count_props = 0;
        return copy_to_user((void *)argp, &op, sizeof(op)) ? -EFAULT : 0;
    }
    case DRM_NR_MODE_GETPROPERTY:
    case DRM_NR_MODE_CURSOR:    return -EINVAL; /* no properties / KMS cursor yet */
    default: break;
    }
    if (!virtio_gpu_is_active()) return -ENODEV;

    /* KMS + dumb buffers + PRIME work on 2D-only hosts too */
    {
        DrmClient *kc = client_get(slot);
        if (!kc) return -ENOMEM;
        switch (nr) {
        case DRM_NR_MODE_SETCRTC:      return ioctl_setcrtc(*slot, argp);
        case DRM_NR_MODE_ADDFB:        return ioctl_addfb(*slot, argp);
        case DRM_NR_MODE_ADDFB2:       return ioctl_addfb2(*slot, argp);
        case DRM_NR_MODE_RMFB:         return ioctl_rmfb(*slot, argp);
        case DRM_NR_MODE_DIRTYFB:      return ioctl_dirtyfb(*slot, argp);
        case DRM_NR_MODE_PAGE_FLIP:    return ioctl_page_flip(*slot, kc, argp);
        case DRM_NR_MODE_CREATE_DUMB:  return ioctl_create_dumb(*slot, argp);
        case DRM_NR_MODE_MAP_DUMB:     return ioctl_map_dumb(*slot, argp);
        case DRM_NR_MODE_DESTROY_DUMB: return ioctl_gem_close(*slot, kc, argp); /* same layout: u32 handle */
        case DRM_NR_PRIME_HANDLE_TO_FD: return ioctl_prime_handle_to_fd(*slot, argp);
        case DRM_NR_PRIME_FD_TO_HANDLE: return ioctl_prime_fd_to_handle(*slot, kc, argp);
        case DRM_NR_GEM_CLOSE:         return ioctl_gem_close(*slot, kc, argp);
        default: break;
        }
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
    /* its framebuffers (and the screen, if it had it) */
    for (uint32_t i = 0; i < DRM_MAX_FBS; i++) {
        if (g_fbs[i].used && g_fbs[i].owner == slot) fb_remove(&g_fbs[i]);
    }
    for (uint32_t i = 0; i < DRM_MAX_BOS; i++) {
        if (g_bos[i].used && (g_bos[i].clients & (1u << (slot - 1))))
            bo_release_client(slot, c, &g_bos[i]);
    }
    for (uint32_t i = 0; i < DRM_MAX_BOS; i++) {
        if (g_bos[i].used && g_bos[i].zombie) bo_destroy(&g_bos[i]);
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
