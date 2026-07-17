#pragma once
#include <linux/types.h>
#include <stdint.h>
#include <stddef.h>

struct dma_buf_export_sync_file {
    __u32 flags;
    __s32 fd;
};

#define DMA_BUF_SYNC_READ (1 << 0)
#define DMA_BUF_SYNC_WRITE (2 << 0)
#define DMA_BUF_SYNC_RW (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)

#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE 0x1234
