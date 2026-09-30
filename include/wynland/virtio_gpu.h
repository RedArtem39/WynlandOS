/*
 * WynlandOS - Virtio-GPU Header
 */
#pragma once

#include <wynland/types.h>
#include <wynland/virtio.h>

/* Virtio-GPU command types */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO          0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D        0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF            0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT               0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH            0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D       0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING   0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING   0x0107
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO           0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET                0x0109

/* 3D commands (VIRTIO_GPU_F_VIRGL) */
#define VIRTIO_GPU_CMD_CTX_CREATE                0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY               0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE       0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE       0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D        0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D       0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D     0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D                 0x0207
#define VIRTIO_GPU_CMD_UPDATE_CURSOR             0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR               0x0301

/* Virtio-GPU response types */
#define VIRTIO_GPU_RESP_OK_NODATA                0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO          0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO           0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET                0x1103

/* Feature bits (word 0) */
#define VIRTIO_GPU_F_VIRGL                       (1u << 0)
#define VIRTIO_GPU_F_EDID                        (1u << 1)
#define VIRTIO_GPU_F_RESOURCE_UUID               (1u << 2)
#define VIRTIO_GPU_F_RESOURCE_BLOB               (1u << 3)
#define VIRTIO_GPU_F_CONTEXT_INIT                (1u << 4)

/* Capset ids */
#define VIRTIO_GPU_CAPSET_VIRGL                  1
#define VIRTIO_GPU_CAPSET_VIRGL2                 2
#define VIRTIO_GPU_CAPSET_VENUS                  4
#define VIRTIO_GPU_RESP_ERR_UNSPEC               0x1200

/* Pixel formats */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM         1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM         2

typedef struct PACKED {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} VirtioGpuCtrlHeader;

typedef struct PACKED {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} VirtioGpuCtrlResponse;

/* Pos for cursor update */
typedef struct PACKED {
    uint32_t scanout_id;
    uint32_t x;
    uint32_t y;
    uint32_t padding;
} VirtioGpuPos;

typedef struct PACKED {
    VirtioGpuCtrlHeader hdr;
    VirtioGpuPos pos;
    uint32_t resource_id;
    uint32_t hot_x;
    uint32_t hot_y;
    uint32_t padding;
} VirtioGpuUpdateCursor;

/* Command specific payloads */
typedef struct PACKED {
    VirtioGpuCtrlHeader hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} VirtioGpuResourceCreate2d;

typedef struct PACKED {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} VirtioGpuMemEntry;

typedef struct PACKED {
    VirtioGpuCtrlHeader hdr;
    uint32_t resource_id;
    uint32_t num_entries;
    VirtioGpuMemEntry entries[1];
} VirtioGpuResourceAttachBacking;

typedef struct PACKED {
    VirtioGpuCtrlHeader hdr;
    uint32_t rxx, ryy, rww, rhh;
    uint32_t scanout_id;
    uint32_t resource_id;
} VirtioGpuSetScanout;

typedef struct PACKED {
    VirtioGpuCtrlHeader hdr;
    uint32_t rxx, ryy, rww, rhh;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} VirtioGpuTransferToHost2d;

typedef struct PACKED {
    VirtioGpuCtrlHeader hdr;
    uint32_t rxx, ryy, rww, rhh;
    uint32_t resource_id;
    uint32_t padding;
} VirtioGpuResourceFlush;

typedef struct PACKED {
    VirtioGpuCtrlHeader hdr;
    uint32_t capset_index;
    uint32_t padding;
} VirtioGpuGetCapsetInfo;

typedef struct PACKED {
    VirtioGpuCtrlHeader hdr;
    uint32_t capset_id;
    uint32_t capset_max_version;
    uint32_t capset_max_size;
    uint32_t padding;
} VirtioGpuRespCapsetInfo;

typedef struct PACKED {
    VirtioGpuCtrlHeader hdr;
    uint32_t capset_id;
    uint32_t capset_version;
} VirtioGpuGetCapset;

typedef struct {
    uint32_t capset_id;
    uint32_t max_version;
    uint32_t max_size;
} VirtioGpuCapsetInfo;

/* hdr.flags: the device answers this command only once the host GPU
   has finished it (virgl fence retire) -- that IS our completion signal */
#define VIRTIO_GPU_FLAG_FENCE 1u

#define VIRTIO_GPU_CMD_RESOURCE_UNREF_ID VIRTIO_GPU_CMD_RESOURCE_UNREF

typedef struct PACKED {
    VirtioGpuCtrlHeader hdr;
    uint32_t nlen;
    uint32_t context_init;
    char     debug_name[64];
} VirtioGpuCtxCreate;

typedef struct PACKED {
    VirtioGpuCtrlHeader hdr;
    uint32_t resource_id;
    uint32_t padding;
} VirtioGpuCtxResource;   /* CTX_ATTACH/DETACH_RESOURCE, RESOURCE_UNREF */

typedef struct PACKED {
    VirtioGpuCtrlHeader hdr;
    uint32_t resource_id;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;
} VirtioGpuResourceCreate3d;

typedef struct PACKED {
    uint32_t x, y, z, w, h, d;
} VirtioGpuBox;

typedef struct PACKED {
    VirtioGpuCtrlHeader hdr;
    VirtioGpuBox box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
} VirtioGpuTransferHost3d;

typedef struct PACKED {
    VirtioGpuCtrlHeader hdr;
    uint32_t size;
    uint32_t padding;
} VirtioGpuCmdSubmit;     /* followed by `size` bytes of virgl command stream */

typedef struct PACKED {
    VirtioGpuCtrlHeader hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    VirtioGpuMemEntry entry; /* one physically contiguous range */
} VirtioGpuAttachBacking1;

/* Control-queue request ticket (see virtio_gpu.c's "Control queue"). */
typedef struct {
    uint16_t head;
    uint64_t seq;
} VgpuTicket;

/* Queue one command: `cmd` then optional `data` (device-readable), then
   `resp` (device-writable). Buffers must stay valid until the ticket is
   done. Interrupts must be off (syscall context). */
bool vgpu_submit(const void *cmd, uint32_t cmd_len, const void *data, uint32_t data_len,
                 void *resp, uint32_t resp_len, VgpuTicket *t);
bool vgpu_done(VgpuTicket t);   /* reaps first */
uint32_t vgpu_alloc_resource_id(void);
uint32_t vgpu_alloc_ctx_id(void);
uint64_t vgpu_next_fence_id(void);

#define VIRTIO_GPU_MAX_CAPSETS 8
#define VIRTIO_GPU_CAPSET_BUF  4096

/* 3D state, valid after virtio_gpu_init() */
extern bool g_virgl;        /* host accepted VIRTIO_GPU_F_VIRGL */
extern bool g_context_init; /* host accepted VIRTIO_GPU_F_CONTEXT_INIT */
uint32_t virtio_gpu_capset_count(void);
const VirtioGpuCapsetInfo *virtio_gpu_capset(uint32_t i);
int virtio_gpu_get_capset(uint32_t id, uint32_t version, void *out, uint32_t len);

/* API functions */
bool virtio_gpu_init(void);
void virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void virtio_gpu_update_cursor(uint32_t resource_id, uint32_t x, uint32_t y);
/* Hardware cursor plane: position straight from the mouse IRQ, shape
   0=arrow 1=hand 2=text 3=resize (pre-uploaded, switching is free). */
void virtio_gpu_move_cursor(uint32_t x, uint32_t y);
void virtio_gpu_set_cursor_shape(uint32_t shape);
void virtio_gpu_tick(void); /* 1 kHz timer hook */
bool virtio_gpu_is_active(void);
