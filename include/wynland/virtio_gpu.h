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
#define VIRTIO_GPU_CMD_UPDATE_CURSOR             0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR               0x0301

/* Virtio-GPU response types */
#define VIRTIO_GPU_RESP_OK_NODATA                0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO          0x1101
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
