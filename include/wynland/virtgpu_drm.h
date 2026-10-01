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
/* A dma-buf (PRIME) fd: current_cluster = buffer object index + 1. */
#define DRM_PRIME_FD   0xFFFFFFD2

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
uint32_t drm_open_client(void);       /* open() of a DRM node: the fd's client */
void drm_client_ref(uint32_t slot);   /* a copied fd shares the client */

/* dma-buf fds: reference counting (dup/fork/SCM_RIGHTS take one, close
   drops one) and mmap. */
void drm_prime_get(uint32_t idx);
void drm_prime_put(uint32_t idx);
int  drm_prime_mmap_lookup(uint32_t idx, uint64_t offset, uint64_t len, uint64_t *phys);

/* Page-flip events on a DRM fd: read() returns struct drm_event_vblank
   records; poll() is readable once one is ready. */
int64_t drm_read(uint32_t slot, uint64_t ubuf, uint64_t len, bool nonblock);
bool    drm_poll_ready(uint32_t slot);
struct WaitQueue *drm_event_wq(uint32_t slot);
void    drm_tick(void); /* 1 kHz, IRQ context */

/* Provided by kernel/syscall.c: install a dma-buf fd for BO `idx` in the
   current process (returns the fd or -errno), and resolve an fd back to
   its BO index (0 if it isn't a dma-buf). */
int      drm_install_prime_fd(uint32_t idx, bool cloexec);
uint32_t drm_prime_fd_bo(int fd);
