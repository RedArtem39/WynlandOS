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
