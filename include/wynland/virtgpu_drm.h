/*
 * WynlandOS - DRM render node for virtio-gpu 3D (virgl)
 * ============================================================
 * The Linux virtio_gpu DRM uAPI (include/uapi/drm/virtgpu_drm.h), enough
 * for Mesa's unmodified virgl Gallium driver to run on /dev/dri/renderD128.
 */
#pragma once

#include <wynland/types.h>

/* VfsFile.node.first_cluster sentinels for the two DRM nodes. */
#define DRM_DEV_CARD   0xFFFFFFD0
#define DRM_DEV_RENDER 0xFFFFFFD1
#define IS_DRM_DEV(fc) ((fc) == DRM_DEV_CARD || (fc) == DRM_DEV_RENDER)

/* ioctl on a DRM fd. `slot` is the VfsFile's current_cluster (0 = not yet
   bound to a DRM client; the call binds it). Returns 0 or -errno. */
int64_t drm_ioctl(uint32_t *slot, uint64_t request, uint64_t argp);

/* mmap on a DRM fd: resolve a VIRTGPU_MAP offset. On success fills the
   BO's physical base and size and returns 0. */
int drm_mmap_lookup(uint32_t slot, uint64_t offset, uint64_t len, uint64_t *phys);

/* Close of a DRM fd in process `pid`. Only the opener's close releases
   the client's GPU objects (fork()ed copies of the fd share the slot).
   can_sleep=false (process teardown) defers the release to the next
   DRM ioctl. */
void drm_release(uint32_t slot, uint64_t pid, bool can_sleep);
