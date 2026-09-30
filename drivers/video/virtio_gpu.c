/*
 * WynlandOS - Virtio-GPU Display Driver Implementation
 * ============================================================
 * Legacy/Transitional PCI driver for Virtio-GPU.
 * Communicates with QEMU's virtio-gpu-pci device using Port I/O.
 */

#include <wynland/virtio_gpu.h>
#include <wynland/pci.h>
#include <wynland/pmm.h>
#include <wynland/vmm.h>

extern void serial_write_string(const char *str);
extern void uint_to_str(uint64_t val, char *buf);
extern void uint_to_hex(uint64_t val, char *buf);
extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);

extern uint32_t *comp_get_backbuffer(void);
extern uint32_t  comp_get_width(void);
extern uint32_t  comp_get_height(void);

extern void *kmalloc(size_t size);
extern void  kfree(void *ptr);

static uint8_t gpu_pci_bus = 0;
static uint8_t gpu_pci_slot = 0;
static uint8_t gpu_pci_func = 0;

/* PCI IDs for VirtIO */
#define VIRTIO_VENDOR_ID         0x1AF4
#define VIRTIO_GPU_DEVICE_LEGACY 0x1050  /* Transitional/Legacy GPU ID (0x1050) */

/* VirtIO 1.0 Modern MMIO Offsets in BAR4 */
#define VIRTIO_MODERN_DEV_FEATURE_SEL   0x00
#define VIRTIO_MODERN_DEV_FEATURE       0x04
#define VIRTIO_MODERN_DRV_FEATURE_SEL   0x08
#define VIRTIO_MODERN_DRV_FEATURE       0x0C
#define VIRTIO_MODERN_QUEUE_SEL     0x16
#define VIRTIO_MODERN_QUEUE_SIZE    0x18
#define VIRTIO_MODERN_QUEUE_ENABLE  0x1C
#define VIRTIO_MODERN_QUEUE_DESC    0x20
#define VIRTIO_MODERN_QUEUE_DRIVER  0x28
#define VIRTIO_MODERN_QUEUE_DEVICE  0x30
#define VIRTIO_MODERN_STATUS        0x14
#define VIRTIO_MODERN_NOTIFY_BASE   0x3000



/* PCI command register bits */
#define PCI_CMD_BUS_MASTER       0x04

/* Control and Cursor Queues */
#define CTRL_QUEUE   0
#define CURSOR_QUEUE 1

static volatile uint8_t *mmio_base = NULL;
static volatile uint8_t *mmio_notify = NULL;
static volatile uint16_t *ctrl_q_notify = NULL;
static volatile uint16_t *cursor_q_notify = NULL;
static uint32_t notify_off_multiplier = 0;
static bool initialized = false;

static Virtqueue ctrl_q;
static Virtqueue cursor_q;

#define VQ_MAX_SIZE 256

/* The device writes used->idx behind the compiler's back: every poll of it
   must be a real load. */
#define VQ_USED_IDX(q) (*(volatile uint16_t *)&(q).used->idx)

/* Static commands/responses in contiguous kernel memory */
static VirtioGpuResourceCreate2d      cmd_create;
static VirtioGpuCtrlResponse          resp_create;

typedef struct PACKED {
    VirtioGpuCtrlHeader hdr;
    uint32_t resource_id;
    uint32_t num_entries;
    VirtioGpuMemEntry entries[4096];
} FbAttachCmd;

typedef struct PACKED {
    VirtioGpuCtrlHeader hdr;
    uint32_t resource_id;
    uint32_t num_entries;
    VirtioGpuMemEntry entries[4];
} CursorAttachCmd;

static VirtioGpuMemEntry fb_entries[4096] __attribute__((aligned(4096)));
static VirtioGpuResourceAttachBacking fb_attach_hdr     __attribute__((aligned(4096)));
static VirtioGpuResourceAttachBacking cursor_attach_hdr __attribute__((aligned(4096)));
static VirtioGpuCtrlResponse resp_attach;

static VirtioGpuSetScanout            cmd_scanout;
static VirtioGpuCtrlResponse          resp_scanout;

static VirtioGpuTransferToHost2d cmd_transfer;
static VirtioGpuCtrlResponse          resp_transfer;

static VirtioGpuResourceFlush    cmd_flush;
static VirtioGpuCtrlResponse          resp_flush;
static void flush_resolve_phys(void);

/* MMIO Register Access Helpers */
static inline void mmio_write8(uint32_t offset, uint8_t val) {
    *(volatile uint8_t *)(mmio_base + offset) = val;
}

static inline void mmio_write16(uint32_t offset, uint16_t val) {
    *(volatile uint16_t *)(mmio_base + offset) = val;
}

static inline void mmio_write32(uint32_t offset, uint32_t val) {
    *(volatile uint32_t *)(mmio_base + offset) = val;
}

static inline uint8_t mmio_read8(uint32_t offset) {
    return *(volatile uint8_t *)(mmio_base + offset);
}

static inline uint16_t mmio_read16(uint32_t offset) {
    return *(volatile uint16_t *)(mmio_base + offset);
}

static inline uint32_t mmio_read32(uint32_t offset) {
    return *(volatile uint32_t *)(mmio_base + offset);
}

/* Logging Helpers */
static void log_str(const char *msg) {
    serial_write_string(msg);
}

static void log_hex(const char *prefix, uint64_t val) {
    char buf[20];
    serial_write_string(prefix);
    uint_to_hex(val, buf);
    serial_write_string(buf);
    serial_write_string("\r\n");
}

static void log_dec(const char *prefix, uint64_t val) {
    char buf[24];
    serial_write_string(prefix);
    uint_to_str(val, buf);
    serial_write_string(buf);
    serial_write_string("\r\n");
}

/* ============================================================
 * PCI Discovery
 * ============================================================ */

static uint64_t pci_get_bar_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar_idx)
{
    uint8_t offset = 0x10 + bar_idx * 4;
    uint32_t bar = pci_read_config(bus, slot, func, offset);

    if (bar & 1) {
        /* Port I/O BAR */
        return bar & ~0x3U;
    } else {
        /* Memory BAR */
        uint8_t type = (bar >> 1) & 3;
        if (type == 2) {
            /* 64-bit BAR: combine with next BAR */
            uint32_t bar_hi = pci_read_config(bus, slot, func, offset + 4);
            return (((uint64_t)bar_hi << 32) | (bar & ~0xFU));
        } else {
            /* 32-bit BAR */
            return bar & ~0xFU;
        }
    }
}

/* struct virtio_gpu_config: events_read, events_clear, num_scanouts,
   num_capsets (all le32). NULL if the device didn't expose it. */
static volatile uint8_t *mmio_devcfg = NULL;

static volatile uint8_t *virtio_gpu_pci_find(void)
{
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t reg0 = pci_read_config((uint8_t)bus, slot, func, 0x00);
                uint16_t vendor = reg0 & 0xFFFF;

                if (vendor == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }

                if (vendor != VIRTIO_VENDOR_ID)
                    continue;

                uint16_t device_id = (reg0 >> 16) & 0xFFFF;
                if (device_id != VIRTIO_GPU_DEVICE_LEGACY)
                    continue;

                log_str("VIRTIO-GPU: Found legacy/transitional device on PCI\r\n");

                /* Print raw BAR values for debugging */
                for (uint8_t bar_idx = 0; bar_idx < 6; bar_idx++) {
                    uint32_t bar = pci_read_config((uint8_t)bus, slot, func, 0x10 + bar_idx * 4);
                    log_hex("VIRTIO-GPU: Raw BAR = ", bar);
                }

                /* Parse PCI Capabilities to find VirtIO 1.0 regions */
                uint32_t status_reg = pci_read_config((uint8_t)bus, slot, func, 0x04);
                uint64_t common_phys = 0;
                uint64_t notify_phys = 0;
                uint64_t devcfg_phys = 0;

                if (status_reg & (1 << 20)) { // Capabilities List flag in Status Register
                    uint32_t cap_ptr = pci_read_config((uint8_t)bus, slot, func, 0x34) & 0xFF;
                    log_hex("VIRTIO-GPU: Capabilities List starts at offset ", cap_ptr);

                    while (cap_ptr != 0) {
                        uint32_t cap_header = pci_read_config((uint8_t)bus, slot, func, cap_ptr);
                        uint8_t cap_id = cap_header & 0xFF;
                        uint8_t next_ptr = (cap_header >> 8) & 0xFF;

                        if (cap_id == 0x09) { // Vendor-specific capability (VirtIO)
                            uint8_t cfg_type = (cap_header >> 24) & 0xFF;
                            uint32_t bar_reg = pci_read_config((uint8_t)bus, slot, func, cap_ptr + 4);
                            uint8_t bar = bar_reg & 0xFF;
                            uint32_t offset = pci_read_config((uint8_t)bus, slot, func, cap_ptr + 8);
                            uint32_t length = pci_read_config((uint8_t)bus, slot, func, cap_ptr + 12);

                            uint64_t bar_phys = pci_get_bar_addr((uint8_t)bus, slot, func, bar);

                            log_dec("VIRTIO-GPU: Found VirtIO Capability Type = ", cfg_type);
                            log_dec("  BAR = ", bar);
                            log_hex("  Offset = ", offset);
                            log_hex("  Length = ", length);

                            if (cfg_type == 1) { // VIRTIO_PCI_CAP_COMMON_CFG
                                common_phys = bar_phys + offset;
                            } else if (cfg_type == 2) { // VIRTIO_PCI_CAP_NOTIFY_CFG
                                notify_phys = bar_phys + offset;
                                notify_off_multiplier = pci_read_config((uint8_t)bus, slot, func, cap_ptr + 16);
                            } else if (cfg_type == 4) { // VIRTIO_PCI_CAP_DEVICE_CFG
                                devcfg_phys = bar_phys + offset;
                            }
                        }

                        cap_ptr = next_ptr;
                    }
                } else {
                    log_str("VIRTIO-GPU: No PCI Capabilities List found!\r\n");
                }

                if (common_phys == 0 || notify_phys == 0) {
                    log_str("VIRTIO-GPU: ERROR - VirtIO 1.0 Common or Notify regions not found!\r\n");
                    continue;
                }

                log_hex("VIRTIO-GPU: Common Config physical address = ", common_phys);
                log_hex("VIRTIO-GPU: Notify physical address = ", notify_phys);

                /* Enable MMIO and Bus Mastering */
                gpu_pci_bus = (uint8_t)bus;
                gpu_pci_slot = slot;
                gpu_pci_func = func;
                uint32_t cmd = pci_read_config((uint8_t)bus, slot, func, 0x04);
                cmd |= 0x06; /* PCI_CMD_BUS_MASTER (0x04) | PCI_CMD_MEMORY_SPACE (0x02) */
                pci_write_config((uint8_t)bus, slot, func, 0x04, cmd);

                /* Map the MMIO regions */
                extern void vmm_map_mmio(uint64_t phys_addr, uint64_t size);
                vmm_map_mmio(common_phys & ~4095ULL, 4096);
                vmm_map_mmio(notify_phys & ~4095ULL, 4096);

                mmio_notify = (volatile uint8_t *)(uintptr_t)notify_phys;
                if (devcfg_phys) {
                    vmm_map_mmio(devcfg_phys & ~4095ULL, 4096);
                    mmio_devcfg = (volatile uint8_t *)(uintptr_t)devcfg_phys;
                }

                log_str("VIRTIO-GPU: MMIO mapped and Bus mastering enabled\r\n");
                return (volatile uint8_t *)(uintptr_t)common_phys;
            }
        }
    }
    return NULL;
}

/* ============================================================
 * Virtqueue Setup
 * ============================================================ */

static bool virtqueue_setup(Virtqueue *vq, uint16_t queue_idx)
{
    /* Select queue */
    mmio_write16(VIRTIO_MODERN_QUEUE_SEL, queue_idx);

    /* Read queue size */
    uint16_t qsz = mmio_read16(VIRTIO_MODERN_QUEUE_SIZE);
    if (qsz == 0 || qsz == 0xFFFF) {
        log_dec("VIRTIO-GPU: ERROR - Queue size is invalid: ", qsz);
        return false;
    }
    /* A VirtIO 1.0 driver may shrink the queue; cap it so the per-
       descriptor cursor command slots below always cover every index. */
    if (qsz > VQ_MAX_SIZE) {
        qsz = VQ_MAX_SIZE;
        mmio_write16(VIRTIO_MODERN_QUEUE_SIZE, qsz);
    }

    vq->size = qsz;
    log_dec("VIRTIO-GPU: Setup Queue index = ", queue_idx);
    log_dec("VIRTIO-GPU: Queue size = ", qsz);

    /* Calculate layout for VirtIO 1.0 (modern can use byte alignments) */
    uint32_t desc_size  = 16 * (uint32_t)qsz;
    uint32_t avail_size = 6 + 2 * (uint32_t)qsz;
    uint32_t used_offset = (desc_size + avail_size + 15) & ~15U;
    uint32_t used_size  = 6 + 8 * (uint32_t)qsz;
    uint32_t total_size = used_offset + used_size;

    uint32_t pages_needed = (total_size + 4095) / 4096;
    /* One physically contiguous block: the device gets ONE base address
       per ring, so back-to-back pmm_alloc_page() calls that merely
       happen to be adjacent are not good enough. */
    uint8_t *mem = (uint8_t *)pmm_alloc_contiguous(pages_needed);
    if (!mem) {
        return false;
    }

    memset(mem, 0, pages_needed * 4096);

    vq->desc  = (VirtqDesc *)mem;
    vq->avail = (VirtqAvail *)(mem + desc_size);
    vq->used  = (VirtqUsed *)(mem + used_offset);

    for (uint16_t i = 0; i < qsz; i++) {
        vq->desc[i].next = i + 1;
        vq->desc[i].flags = (i < (uint16_t)(qsz - 1)) ? VIRTQ_DESC_F_NEXT : 0;
    }
    vq->free_head = 0;
    vq->num_free  = qsz;
    vq->last_used = 0;

    /* Get physical addresses */
    PageTable *pml4 = vmm_get_current_pml4();
    uint64_t desc_phys  = vmm_get_phys(pml4, (uint64_t)(uintptr_t)vq->desc);
    uint64_t avail_phys = vmm_get_phys(pml4, (uint64_t)(uintptr_t)vq->avail);
    uint64_t used_phys  = vmm_get_phys(pml4, (uint64_t)(uintptr_t)vq->used);

    /* Write physical addresses directly (VirtIO 1.0 supports 64-bit addresses) */
    mmio_write32(VIRTIO_MODERN_QUEUE_DESC, (uint32_t)desc_phys);
    mmio_write32(VIRTIO_MODERN_QUEUE_DESC + 4, (uint32_t)(desc_phys >> 32));

    mmio_write32(VIRTIO_MODERN_QUEUE_DRIVER, (uint32_t)avail_phys);
    mmio_write32(VIRTIO_MODERN_QUEUE_DRIVER + 4, (uint32_t)(avail_phys >> 32));

    mmio_write32(VIRTIO_MODERN_QUEUE_DEVICE, (uint32_t)used_phys);
    mmio_write32(VIRTIO_MODERN_QUEUE_DEVICE + 4, (uint32_t)(used_phys >> 32));

    /* Enable queue */
    mmio_write16(VIRTIO_MODERN_QUEUE_ENABLE, 1);

    /* Read queue notify offset (offset 0x1E) */
    uint16_t notify_off = mmio_read16(0x1E);
    volatile uint16_t *notify_ptr = (volatile uint16_t *)(mmio_notify + notify_off * notify_off_multiplier);

    if (queue_idx == CTRL_QUEUE) {
        ctrl_q_notify = notify_ptr;
    } else {
        cursor_q_notify = notify_ptr;
    }

    return true;
}

static uint16_t vq_alloc_desc(Virtqueue *vq)
{
    if (vq->num_free == 0)
        return 0xFFFF;

    uint16_t idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->num_free--;
    return idx;
}

static void vq_free_desc(Virtqueue *vq, uint16_t idx)
{
    vq->desc[idx].next = vq->free_head;
    vq->desc[idx].flags = VIRTQ_DESC_F_NEXT;
    vq->free_head = idx;
    vq->num_free++;
}

/* ============================================================
 * Control queue: asynchronous requests
 * ============================================================
 * Every command is a request: a descriptor chain whose head index names it.
 * ctrlq_submit() queues it and returns a ticket; ctrlq_reap() walks the used
 * ring and retires completed chains BY HEAD INDEX (the device may complete
 * out of submission order once 3D fences are involved -- a SUBMIT_3D with a
 * fence is answered only when the host GPU is done with it, while later
 * 2D commands already complete). Nothing may assume "N submitted, so used
 * idx + N means mine are done" any more.
 *
 * Tickets are (head, seq): seq grows by one per submission and
 * ctrl_done_seq[head] records the seq of the last request retired through
 * that head -- monotonic, so "done_seq[head] >= my seq" stays true even
 * after the head is reused.
 *
 * Callers run with interrupts off (syscall context, or irq_save()); the
 * timer tick also reaps. Uniprocessor: interrupts-off IS the lock. */

#define CTRLQ_MAX_SEGS 72 /* 256 KB command buffer in 4 KB pages + hdr + resp */

typedef struct {
    uint64_t phys;
    uint32_t len;
    bool     device_writes; /* response buffer */
} CtrlSeg;

typedef VgpuTicket CtrlTicket; /* public name: include/wynland/virtio_gpu.h */

static uint64_t ctrl_seq = 0;
static uint64_t ctrl_done_seq[VQ_MAX_SIZE];
static uint64_t ctrl_inflight_seq[VQ_MAX_SIZE]; /* seq of the chain at head */
static uint8_t  ctrl_chain_len[VQ_MAX_SIZE];

static uint64_t irq_save(void);
static void irq_restore(uint64_t rflags);

/* Retire every chain the device has returned. */
static void ctrlq_reap(void)
{
    uint64_t fl = irq_save();
    while (VQ_USED_IDX(ctrl_q) != ctrl_q.last_used) {
        __asm__ volatile("" ::: "memory"); /* ring entry valid once idx says so */
        uint16_t slot = ctrl_q.last_used % ctrl_q.size;
        uint16_t head = (uint16_t)ctrl_q.used->ring[slot].id;
        uint16_t d = head;
        for (uint8_t i = 0; i < ctrl_chain_len[head]; i++) {
            uint16_t next = ctrl_q.desc[d].next;
            vq_free_desc(&ctrl_q, d);
            d = next;
        }
        ctrl_done_seq[head] = ctrl_inflight_seq[head];
        ctrl_q.last_used++;
    }
    irq_restore(fl);
}

static bool ctrlq_done(CtrlTicket t)
{
    return ctrl_done_seq[t.head] >= t.seq;
}

/* Queue one request. Returns false (nothing queued) if the ring doesn't
   have enough free descriptors even after reaping. */
static bool ctrlq_submit(const CtrlSeg *segs, int n, CtrlTicket *out)
{
    if (n <= 0 || n > CTRLQ_MAX_SEGS) return false;
    uint64_t fl = irq_save();
    if (ctrl_q.num_free < n) {
        irq_restore(fl);
        ctrlq_reap();
        fl = irq_save();
        if (ctrl_q.num_free < n) { irq_restore(fl); return false; }
    }

    uint16_t d[CTRLQ_MAX_SEGS];
    for (int i = 0; i < n; i++) d[i] = vq_alloc_desc(&ctrl_q);
    for (int i = 0; i < n; i++) {
        ctrl_q.desc[d[i]].addr  = segs[i].phys;
        ctrl_q.desc[d[i]].len   = segs[i].len;
        ctrl_q.desc[d[i]].flags = (segs[i].device_writes ? VIRTQ_DESC_F_WRITE : 0) |
                                  (i + 1 < n ? VIRTQ_DESC_F_NEXT : 0);
        ctrl_q.desc[d[i]].next  = (i + 1 < n) ? d[i + 1] : 0;
    }
    uint16_t head = d[0];
    ctrl_chain_len[head] = (uint8_t)n;
    ctrl_inflight_seq[head] = ++ctrl_seq;

    uint16_t avail = ctrl_q.avail->idx;
    ctrl_q.avail->ring[avail % ctrl_q.size] = head;
    __asm__ volatile("mfence" ::: "memory");
    ctrl_q.avail->idx = (uint16_t)(avail + 1);
    __asm__ volatile("mfence" ::: "memory");
    *ctrl_q_notify = CTRL_QUEUE;

    out->head = head;
    out->seq = ctrl_seq;
    irq_restore(fl);
    return true;
}

/* Append the pages backing kernel buffer [buf, buf+len) as segments --
   a kernel virtual range needn't be physically contiguous past a page. */
static int ctrlq_add_buf(CtrlSeg *segs, int n, const void *buf, uint32_t len, bool device_writes)
{
    PageTable *pml4 = vmm_get_current_pml4();
    uint64_t va = (uint64_t)(uintptr_t)buf;
    while (len > 0) {
        if (n >= CTRLQ_MAX_SEGS) return -1;
        uint32_t chunk = 4096 - (uint32_t)(va & 4095);
        if (chunk > len) chunk = len;
        segs[n].phys = vmm_get_phys(pml4, va);
        segs[n].len = chunk;
        segs[n].device_writes = device_writes;
        n++;
        va += chunk;
        len -= chunk;
    }
    return n;
}

/* TSC, calibrated against the 1 kHz clock in virtio_gpu_tick(): the only
   clock that runs while interrupts are off. */
static uint64_t tsc_per_ms = 0;

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#define CTRLQ_SPIN_FALLBACK 2000000 /* iterations per ms-equivalent until calibrated */

/* Spin (interrupts off) until the ticket completes or `ms` pass. */
static bool ctrlq_wait_spin(CtrlTicket t, uint32_t ms)
{
    uint64_t t0 = rdtsc();
    uint64_t limit = tsc_per_ms * ms;
    for (uint64_t i = 0;; i++) {
        ctrlq_reap();
        if (ctrlq_done(t)) return true;
        if (tsc_per_ms ? (rdtsc() - t0 >= limit) : (i >= (uint64_t)CTRLQ_SPIN_FALLBACK * ms / 20))
            return false;
        __asm__ volatile("pause" ::: "memory");
    }
}

bool virtio_gpu_pci_info(VgpuPciInfo *out)
{
    if (!mmio_base) return false;
    uint32_t id  = pci_read_config(gpu_pci_bus, gpu_pci_slot, gpu_pci_func, 0x00);
    uint32_t cls = pci_read_config(gpu_pci_bus, gpu_pci_slot, gpu_pci_func, 0x08);
    uint32_t sub = pci_read_config(gpu_pci_bus, gpu_pci_slot, gpu_pci_func, 0x2C);
    out->bus = gpu_pci_bus;
    out->slot = gpu_pci_slot;
    out->func = gpu_pci_func;
    out->vendor = id & 0xFFFF;
    out->device = id >> 16;
    out->revision = cls & 0xFF;
    out->class_code = cls >> 8;
    out->subvendor = sub & 0xFFFF;
    out->subdevice = sub >> 16;
    return true;
}

/* ---- API for the DRM render node (drivers/video/virtgpu_drm.c) ---- */

bool vgpu_submit(const void *cmd, uint32_t cmd_len, const void *data, uint32_t data_len,
                 void *resp, uint32_t resp_len, VgpuTicket *t)
{
    CtrlSeg segs[CTRLQ_MAX_SEGS];
    int n = ctrlq_add_buf(segs, 0, cmd, cmd_len, false);
    if (n > 0 && data && data_len) n = ctrlq_add_buf(segs, n, data, data_len, false);
    if (n > 0) n = ctrlq_add_buf(segs, n, resp, resp_len, true);
    if (n <= 0) return false;
    return ctrlq_submit(segs, n, t);
}

bool vgpu_done(VgpuTicket t)
{
    ctrlq_reap();
    return ctrlq_done(t);
}

/* Resource ids 1 (scanout) and 2..5 (cursor shapes) are the driver's own. */
static uint32_t next_resource_id = 0x100;
static uint32_t next_ctx_id = 1;
static uint64_t next_fence_id = 1;

uint32_t vgpu_alloc_resource_id(void) { return next_resource_id++; }
uint32_t vgpu_alloc_ctx_id(void)      { return next_ctx_id++; }
uint64_t vgpu_next_fence_id(void)     { return next_fence_id++; }

/* Synchronous command with one command and one response buffer (init-time
   and other rare paths). */
static bool virtio_gpu_send_cmd(void *cmd, uint32_t cmd_len, void *resp, uint32_t resp_len)
{
    CtrlSeg segs[CTRLQ_MAX_SEGS];
    int n = ctrlq_add_buf(segs, 0, cmd, cmd_len, false);
    if (n > 0) n = ctrlq_add_buf(segs, n, resp, resp_len, true);
    CtrlTicket t;
    if (n <= 0 || !ctrlq_submit(segs, n, &t)) return false;
    if (!ctrlq_wait_spin(t, 5000)) {
        log_str("VIRTIO-GPU: Command execution timeout!\r\n");
        return false;
    }
    return true;
}

static bool virtio_gpu_send_split_cmd(void *cmd, uint32_t cmd_len, void *data, uint32_t data_len, void *resp, uint32_t resp_len)
{
    CtrlSeg segs[CTRLQ_MAX_SEGS];
    int n = ctrlq_add_buf(segs, 0, cmd, cmd_len, false);
    if (n > 0) n = ctrlq_add_buf(segs, n, data, data_len, false);
    if (n > 0) n = ctrlq_add_buf(segs, n, resp, resp_len, true);
    CtrlTicket t;
    if (n <= 0 || !ctrlq_submit(segs, n, &t)) return false;
    if (!ctrlq_wait_spin(t, 5000)) {
        log_str("VIRTIO-GPU: Command execution timeout!\r\n");
        return false;
    }
    return true;
}





/* ============================================================
 * Hardware Cursor
 * ============================================================
 * The cursor is a separate host-side plane, exactly like a real GPU's
 * cursor plane: every shape is its own 64x64 ARGB resource uploaded ONCE
 * at init, and a pointer move is a single tiny MOVE_CURSOR command on the
 * dedicated cursor queue -- no framebuffer pixels are touched, nothing is
 * re-transferred, and the position updates straight from the mouse IRQ
 * regardless of how fast (or whether) the compositor is producing frames.
 * QEMU hands this to the host display (GTK/SDL), which draws it as the
 * host's own mouse cursor.
 */

#define CURSOR_DIM        64
#define CURSOR_RES_BASE   2      /* resource ids 2..2+CURSOR_SHAPE_COUNT-1 */
#define CURSOR_SHAPE_COUNT 4     /* arrow, hand, text, resize (compositor.c's
                                    g_current_cursor_type numbering) */

static uint32_t cursor_pixels[CURSOR_SHAPE_COUNT][CURSOR_DIM * CURSOR_DIM]
    __attribute__((aligned(4096)));
static VirtioGpuMemEntry cursor_shape_entries[CURSOR_SHAPE_COUNT][4]
    __attribute__((aligned(4096)));

/* Hotspot (the pixel that IS the pointer position) per shape. */
static const uint8_t cursor_hotspot[CURSOR_SHAPE_COUNT][2] = {
    { 0, 0 },   /* arrow: tip */
    { 7, 0 },   /* hand: index fingertip */
    { 8, 9 },   /* text: I-beam center */
    { 8, 8 },   /* resize: center of the diagonal */
};

/* 1 = black outline, 2 = white fill */
static const uint8_t cursor_mask_arrow[19][18] = {
    { 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0 },
    { 1,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0 },
    { 1,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0 },
    { 1,2,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0 },
    { 1,2,2,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0 },
    { 1,2,2,2,2,2,2,1,1,1,1,1,1,0,0,0,0,0 },
    { 1,2,2,2,1,2,2,1,0,0,0,0,0,0,0,0,0,0 },
    { 1,2,2,1,0,1,2,2,1,0,0,0,0,0,0,0,0,0 },
    { 1,2,1,0,0,1,2,2,1,0,0,0,0,0,0,0,0,0 },
    { 1,1,0,0,0,0,1,2,2,1,0,0,0,0,0,0,0,0 },
    { 1,0,0,0,0,0,1,2,2,1,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0 },
};

static const uint8_t cursor_mask_hand[19][18] = {
    { 0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,1,2,2,2,1,0,1,1,1,0,0,0,0 },
    { 0,0,0,1,1,1,2,2,2,1,1,2,2,2,1,0,0,0 },
    { 0,0,1,2,2,2,1,2,2,2,2,2,2,2,2,1,0,0 },
    { 0,1,2,2,2,2,2,1,2,2,2,2,2,2,2,2,1,0 },
    { 0,1,2,2,2,2,2,2,1,2,2,2,2,2,2,2,1,0 },
    { 1,2,2,2,2,2,2,2,2,1,2,2,2,2,2,2,1,0 },
    { 1,2,2,2,2,2,2,2,2,2,1,2,2,2,2,2,1,0 },
    { 1,2,2,2,2,2,2,2,2,2,2,1,1,1,1,1,0,0 },
    { 1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,0,0 },
    { 0,1,2,2,2,2,2,2,2,2,2,2,2,2,2,1,0,0 },
    { 0,0,1,2,2,2,2,2,2,2,2,2,2,2,1,0,0,0 },
    { 0,0,0,1,2,2,2,2,2,2,2,2,2,1,0,0,0,0 },
    { 0,0,0,0,1,1,2,2,2,2,2,2,1,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,1,1,1,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
};

static const uint8_t cursor_mask_text[19][18] = {
    { 0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0 },
    { 0,0,1,2,2,2,2,2,2,2,2,2,2,2,1,0,0,0 },
    { 0,0,0,1,1,1,1,2,2,2,1,1,1,1,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,1,1,1,1,2,2,2,1,1,1,1,0,0,0,0 },
    { 0,0,1,2,2,2,2,2,2,2,2,2,2,2,1,0,0,0 },
    { 0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
};

static const uint8_t cursor_mask_resize[19][18] = {
    { 1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,2,2,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,2,1,0,1,2,1,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,1,0,0,0,1,2,1,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,1,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,1,2,1,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,1,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,1,2,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,1,2,1,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,1,2,1,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,1,2,1,0,0,1 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,1,2,1 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,2,1 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,1 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,1 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
};

static const uint8_t (*const cursor_masks[CURSOR_SHAPE_COUNT])[18] = {
    cursor_mask_arrow, cursor_mask_hand, cursor_mask_text, cursor_mask_resize,
};

/* Rasterize one mask into its 64x64 B8G8R8A8 (== 0xAARRGGBB little-endian)
   image, with a soft drop shadow offset (+1,+2) behind it -- the shadow
   is a 3x3-weighted blur of the shape's coverage, so it fades out instead
   of being a hard black copy. */
static void cursor_render_shape(uint32_t *dst, const uint8_t (*mask)[18])
{
    memset(dst, 0, CURSOR_DIM * CURSOR_DIM * 4);

    for (int y = 0; y < 24; y++) {
        for (int x = 0; x < 24; x++) {
            uint32_t sum = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int sy = y - 2 + dy;
                    int sx = x - 1 + dx;
                    if (sy < 0 || sy >= 19 || sx < 0 || sx >= 18) continue;
                    if (mask[sy][sx] == 0) continue;
                    sum += (dx == 0 && dy == 0) ? 4 : (dx == 0 || dy == 0) ? 2 : 1;
                }
            }
            /* sum <= 16 -> alpha <= 0x60 */
            uint32_t alpha = sum * 6;
            if (alpha) dst[y * CURSOR_DIM + x] = alpha << 24;
        }
    }

    for (int y = 0; y < 19; y++) {
        for (int x = 0; x < 18; x++) {
            if (mask[y][x] == 1) {
                dst[y * CURSOR_DIM + x] = 0xFF000000; /* black outline */
            } else if (mask[y][x] == 2) {
                dst[y * CURSOR_DIM + x] = 0xFFFFFFFF; /* white fill */
            }
        }
    }
}

/* One command slot PER DESCRIPTOR INDEX: a slot is only rewritten after
   the device has returned its descriptor, so an in-flight command is never
   modified under the host's feet (the old single static command was). 64-
   byte stride keeps every slot inside one page (no split DMA buffer). */
typedef struct {
    VirtioGpuUpdateCursor cmd;
    uint8_t pad[64 - sizeof(VirtioGpuUpdateCursor)];
} CursorCmdSlot;

static CursorCmdSlot cursor_slots[VQ_MAX_SIZE] __attribute__((aligned(4096)));
/* Physical address of each slot, resolved once at init: cursor_submit()
   runs in the mouse IRQ, so it shouldn't walk whatever CR3 happens to be
   loaded on every pointer move. */
static uint64_t cursor_slot_phys[VQ_MAX_SIZE];

static uint32_t cursor_shape = 0;
static uint32_t cursor_x = 0, cursor_y = 0;
static bool     cursor_shape_dirty = false;  /* needs UPDATE_CURSOR, not MOVE */
static volatile bool cursor_pending = false; /* queue was full; resend on tick */

static uint64_t irq_save(void)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    return rflags;
}

static void irq_restore(uint64_t rflags)
{
    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
}

static void cursor_reclaim(void)
{
    while (VQ_USED_IDX(cursor_q) != cursor_q.last_used) {
        /* ring[] entries are only valid once idx says so: don't let the
           compiler hoist the (non-volatile) ring read above the idx load. */
        __asm__ volatile("" ::: "memory");
        uint16_t used_idx = cursor_q.last_used % cursor_q.size;
        uint32_t desc_idx = cursor_q.used->ring[used_idx].id;
        vq_free_desc(&cursor_q, (uint16_t)desc_idx);
        cursor_q.last_used++;
    }
}

/* Interrupts must be off (IRQ context, or irq_save()). */
static void cursor_submit(void)
{
    cursor_reclaim();

    uint16_t desc = vq_alloc_desc(&cursor_q);
    if (desc == 0xFFFF) {
        /* Host hasn't drained the (16-entry) cursor queue yet: remember
           the LATEST state and let virtio_gpu_tick() resend it --
           intermediate positions are worthless anyway. */
        cursor_pending = true;
        return;
    }

    VirtioGpuUpdateCursor *c = &cursor_slots[desc].cmd;
    memset(c, 0, sizeof(*c));
    c->hdr.type = cursor_shape_dirty ? VIRTIO_GPU_CMD_UPDATE_CURSOR
                                     : VIRTIO_GPU_CMD_MOVE_CURSOR;
    c->pos.scanout_id = 0;
    c->pos.x = cursor_x;
    c->pos.y = cursor_y;
    c->resource_id = CURSOR_RES_BASE + cursor_shape;
    c->hot_x = cursor_hotspot[cursor_shape][0];
    c->hot_y = cursor_hotspot[cursor_shape][1];

    cursor_q.desc[desc].addr  = cursor_slot_phys[desc];
    cursor_q.desc[desc].len   = sizeof(*c);
    cursor_q.desc[desc].flags = 0;
    cursor_q.desc[desc].next  = 0;

    uint16_t avail_idx = cursor_q.avail->idx % cursor_q.size;
    cursor_q.avail->ring[avail_idx] = desc;

    __asm__ volatile("mfence" ::: "memory");
    cursor_q.avail->idx++;
    __asm__ volatile("mfence" ::: "memory");

    *cursor_q_notify = CURSOR_QUEUE;

    cursor_shape_dirty = false;
    cursor_pending = false;
}

/* ============================================================
 * Public Driver Interface
 * ============================================================ */

/* ============================================================
 * 3D capability sets (virgl / venus / ...)
 * ============================================================ */

bool g_virgl = false;
bool g_context_init = false;

static VirtioGpuCapsetInfo g_capsets[VIRTIO_GPU_MAX_CAPSETS];
static uint32_t g_num_capsets = 0;

static void virtio_gpu_query_capsets(void)
{
    uint32_t n = mmio_devcfg ? *(volatile uint32_t *)(mmio_devcfg + 12) : 0;
    log_dec("VIRTIO-GPU: host capsets = ", n);
    if (n > VIRTIO_GPU_MAX_CAPSETS) n = VIRTIO_GPU_MAX_CAPSETS;

    static VirtioGpuGetCapsetInfo cmd __attribute__((aligned(64)));
    static VirtioGpuRespCapsetInfo resp __attribute__((aligned(64)));
    for (uint32_t i = 0; i < n; i++) {
        memset(&cmd, 0, sizeof(cmd));
        memset(&resp, 0, sizeof(resp));
        cmd.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
        cmd.capset_index = i;
        if (!virtio_gpu_send_cmd(&cmd, sizeof(cmd), &resp, sizeof(resp)) ||
            resp.hdr.type != VIRTIO_GPU_RESP_OK_CAPSET_INFO) {
            log_hex("VIRTIO-GPU: GET_CAPSET_INFO failed, resp = ", resp.hdr.type);
            continue;
        }
        g_capsets[g_num_capsets].capset_id = resp.capset_id;
        g_capsets[g_num_capsets].max_version = resp.capset_max_version;
        g_capsets[g_num_capsets].max_size = resp.capset_max_size;
        g_num_capsets++;
        log_dec("VIRTIO-GPU:   capset id = ", resp.capset_id);
        log_dec("VIRTIO-GPU:     max_version = ", resp.capset_max_version);
        log_dec("VIRTIO-GPU:     max_size = ", resp.capset_max_size);
    }
}

uint32_t virtio_gpu_capset_count(void) { return g_num_capsets; }

const VirtioGpuCapsetInfo *virtio_gpu_capset(uint32_t i)
{
    return i < g_num_capsets ? &g_capsets[i] : NULL;
}

/* Fetch capset `id`/`version` into `out` (at most `len` bytes). Returns the
   number of bytes copied, or -1. Kernel context; waits synchronously. */
int virtio_gpu_get_capset(uint32_t id, uint32_t version, void *out, uint32_t len)
{
    uint32_t max = 0;
    for (uint32_t i = 0; i < g_num_capsets; i++)
        if (g_capsets[i].capset_id == id) max = g_capsets[i].max_size;
    if (!max || max > VIRTIO_GPU_CAPSET_BUF - sizeof(VirtioGpuCtrlResponse)) return -1;

    /* One physically contiguous page-aligned buffer: the response is a
       header plus up to a few KB of caps, well within it. */
    static uint8_t buf[VIRTIO_GPU_CAPSET_BUF] __attribute__((aligned(4096)));
    static VirtioGpuGetCapset cmd __attribute__((aligned(64)));
    memset(&cmd, 0, sizeof(cmd));
    memset(buf, 0, sizeof(buf));
    cmd.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    cmd.capset_id = id;
    cmd.capset_version = version;

    uint32_t resp_len = sizeof(VirtioGpuCtrlResponse) + max;
    if (!virtio_gpu_send_cmd(&cmd, sizeof(cmd), buf, resp_len)) {
        log_str("VIRTIO-GPU: GET_CAPSET not answered\r\n");
        return -1;
    }
    if (((VirtioGpuCtrlResponse *)buf)->type != VIRTIO_GPU_RESP_OK_CAPSET) {
        log_hex("VIRTIO-GPU: GET_CAPSET resp = ", ((VirtioGpuCtrlResponse *)buf)->type);
        return -1;
    }

    uint32_t n = max < len ? max : len;
    memcpy(out, buf + sizeof(VirtioGpuCtrlResponse), n);
    return (int)n;
}

bool virtio_gpu_init(void)
{
    log_str("VIRTIO-GPU: Initializing VirtIO-GPU driver...\r\n");

    mmio_base = virtio_gpu_pci_find();
    if (mmio_base == NULL) {
        log_str("VIRTIO-GPU: VirtIO-GPU device not found on PCI bus.\r\n");
        return false;
    }

    /* 1. Reset Device and wait for confirmation */
    mmio_write8(VIRTIO_MODERN_STATUS, VIRTIO_STATUS_RESET);
    while (mmio_read8(VIRTIO_MODERN_STATUS) != 0) {
        __asm__ volatile("pause");
    }
    for (volatile int i = 0; i < 100000; i++) {
        __asm__ volatile("pause");
    }

    /* 2. Set ACKNOWLEDGE and DRIVER */
    mmio_write8(VIRTIO_MODERN_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write8(VIRTIO_MODERN_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* 2a. Negotiate features (mandatory for VirtIO 1.0+) */
    /* Select Feature Select 0 */
    mmio_write32(VIRTIO_MODERN_DEV_FEATURE_SEL, 0);
    uint32_t features0 = mmio_read32(VIRTIO_MODERN_DEV_FEATURE);
    
    /* Select Feature Select 1 (contains VIRTIO_F_VERSION_1 at bit 32, i.e. bit 0 of Feature Select 1) */
    mmio_write32(VIRTIO_MODERN_DEV_FEATURE_SEL, 1);
    uint32_t features1 = mmio_read32(VIRTIO_MODERN_DEV_FEATURE);

    /* Accept ONLY what this driver implements. Echoing the device's whole
       offer back used to also accept e.g. VIRTIO_RING_F_EVENT_IDX (bit 29),
       which changes notification rules the virtqueue code doesn't follow.
       VIRGL (3D commands) and CONTEXT_INIT (typed contexts) are taken
       whenever offered -- plain 2D keeps working either way. */
    uint32_t accept0 = features0 & (VIRTIO_GPU_F_VIRGL | VIRTIO_GPU_F_CONTEXT_INIT);
    if (!(features1 & 0x01)) {
        log_str("VIRTIO-GPU: ERROR - device lacks VIRTIO_F_VERSION_1\r\n");
        mmio_write8(VIRTIO_MODERN_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }
    mmio_write32(VIRTIO_MODERN_DRV_FEATURE_SEL, 0);
    mmio_write32(VIRTIO_MODERN_DRV_FEATURE, accept0);

    mmio_write32(VIRTIO_MODERN_DRV_FEATURE_SEL, 1);
    mmio_write32(VIRTIO_MODERN_DRV_FEATURE, 0x01); /* VIRTIO_F_VERSION_1 */

    g_virgl = (accept0 & VIRTIO_GPU_F_VIRGL) != 0;
    g_context_init = (accept0 & VIRTIO_GPU_F_CONTEXT_INIT) != 0;
    log_hex("VIRTIO-GPU: device features[0] = ", features0);
    log_str(g_virgl ? "VIRTIO-GPU: 3D (virgl) available\r\n"
                    : "VIRTIO-GPU: 2D only (host offers no virgl)\r\n");

    /* Set FEATURES_OK status */
    mmio_write8(VIRTIO_MODERN_STATUS, 
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

    /* Check if host accepted features */
    uint8_t status = mmio_read8(VIRTIO_MODERN_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        log_str("VIRTIO-GPU: ERROR - Host rejected features!\r\n");
        mmio_write8(VIRTIO_MODERN_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }
    log_str("VIRTIO-GPU: Features negotiated successfully\r\n");

    /* 3. Setup queues */
    if (!virtqueue_setup(&ctrl_q, CTRL_QUEUE)) {
        log_str("VIRTIO-GPU: ERROR - Failed to setup Control Queue!\r\n");
        mmio_write8(VIRTIO_MODERN_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    if (!virtqueue_setup(&cursor_q, CURSOR_QUEUE)) {
        log_str("VIRTIO-GPU: ERROR - Failed to setup Cursor Queue!\r\n");
        mmio_write8(VIRTIO_MODERN_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    {
        PageTable *pml4 = vmm_get_current_pml4();
        for (uint32_t i = 0; i < VQ_MAX_SIZE; i++)
            cursor_slot_phys[i] = vmm_get_phys(pml4, (uint64_t)(uintptr_t)&cursor_slots[i].cmd);
        flush_resolve_phys();
    }

    /* 4. Set DRIVER_OK preserving FEATURES_OK */
    mmio_write8(VIRTIO_MODERN_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    log_str("VIRTIO-GPU: Virtqueues initialized, status set to DRIVER_OK\r\n");

    /* Ensure Bus Mastering and Memory Space are still enabled in PCI config space */
    uint32_t pci_cmd = pci_read_config(gpu_pci_bus, gpu_pci_slot, gpu_pci_func, 0x04);
    pci_cmd |= 0x06;
    pci_write_config(gpu_pci_bus, gpu_pci_slot, gpu_pci_func, 0x04, pci_cmd);
    log_hex("VIRTIO-GPU: Restored PCI command register status = ", pci_read_config(gpu_pci_bus, gpu_pci_slot, gpu_pci_func, 0x04));

    /* 5. Retrieve screen size and back-buffer address */
    uint32_t w = comp_get_width();
    uint32_t h = comp_get_height();
    uint32_t *back_buffer = comp_get_backbuffer();

    if (!back_buffer) {
        log_str("VIRTIO-GPU: ERROR - Backbuffer not allocated yet!\r\n");
        return false;
    }

    /* 6. Send VIRTIO_GPU_CMD_RESOURCE_CREATE_2D */
    memset(&cmd_create, 0, sizeof(cmd_create));
    cmd_create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd_create.resource_id = 1;
    cmd_create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    cmd_create.width = w;
    cmd_create.height = h;

    if (!virtio_gpu_send_cmd(&cmd_create, sizeof(cmd_create), &resp_create, sizeof(resp_create)) ||
        resp_create.type != VIRTIO_GPU_RESP_OK_NODATA) {
        log_str("VIRTIO-GPU: ERROR - RESOURCE_CREATE_2D failed!\r\n");
        return false;
    }
    log_str("VIRTIO-GPU: 2D Host Resource 1 created\r\n");

    /* 7. Send VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING */
    PageTable *pml4 = vmm_get_current_pml4();
    uint32_t page_count = (w * h * 4 + 4095) / 4096;
    if (page_count > 4096) {
        log_str("VIRTIO-GPU: ERROR - page_count exceeds FbAttachCmd limit!\r\n");
        return false;
    }

    /* Use global fb_attach_hdr */
    memset(&fb_attach_hdr, 0, sizeof(fb_attach_hdr));
    fb_attach_hdr.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    fb_attach_hdr.resource_id = 1;
    fb_attach_hdr.num_entries = page_count;

    uint64_t first_phys_addr = vmm_get_phys(pml4, (uint64_t)(uintptr_t)back_buffer);
    for (uint32_t i = 0; i < page_count; i++) {
        fb_entries[i].addr = first_phys_addr + (uint64_t)i * 4096;
        fb_entries[i].length = 4096;
        fb_entries[i].padding = 0;
    }

    log_dec("VIRTIO-GPU: Attach backing page_count = ", page_count);
    log_hex("  Physical start address = ", first_phys_addr);

    log_str("VIRTIO-GPU: Debug Attach Command Bytes:\r\n");
    uint32_t *debug_ptr = (uint32_t *)&fb_attach_hdr;
    for (int idx = 0; idx < 8; idx++) {
        log_hex("  Word ", debug_ptr[idx]);
    }

    uint32_t data_len = page_count * sizeof(VirtioGpuMemEntry);
    if (!virtio_gpu_send_split_cmd(&fb_attach_hdr, 32, fb_entries, data_len, &resp_attach, sizeof(resp_attach))) {
        log_str("VIRTIO-GPU: ERROR - send_cmd failed (timeout) for main framebuffer attach!\r\n");
        return false;
    }

    if (resp_attach.type != VIRTIO_GPU_RESP_OK_NODATA) {
        log_hex("VIRTIO-GPU: ERROR - RESOURCE_ATTACH_BACKING returned error code: ", resp_attach.type);
        return false;
    }
    log_str("VIRTIO-GPU: Guest back-buffer memory attached to Host Resource 1\r\n");

    /* 8. Send VIRTIO_GPU_CMD_SET_SCANOUT */
    memset(&cmd_scanout, 0, sizeof(cmd_scanout));
    cmd_scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd_scanout.scanout_id = 0;
    cmd_scanout.resource_id = 1;
    cmd_scanout.rxx = 0;
    cmd_scanout.ryy = 0;
    cmd_scanout.rww = w;
    cmd_scanout.rhh = h;

    if (!virtio_gpu_send_cmd(&cmd_scanout, sizeof(cmd_scanout), &resp_scanout, sizeof(resp_scanout)) ||
        resp_scanout.type != VIRTIO_GPU_RESP_OK_NODATA) {
        log_str("VIRTIO-GPU: ERROR - SET_SCANOUT failed!\r\n");
        return false;
    }
    log_str("VIRTIO-GPU: Scanout set to Resource 1\r\n");

    /* Hardware cursor: one 64x64 resource per shape, uploaded once. */
    for (uint32_t shape = 0; shape < CURSOR_SHAPE_COUNT; shape++) {
        uint32_t res_id = CURSOR_RES_BASE + shape;
        cursor_render_shape(cursor_pixels[shape], cursor_masks[shape]);

        memset(&cmd_create, 0, sizeof(cmd_create));
        cmd_create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
        cmd_create.resource_id = res_id;
        cmd_create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
        cmd_create.width = CURSOR_DIM;
        cmd_create.height = CURSOR_DIM;

        if (!virtio_gpu_send_cmd(&cmd_create, sizeof(cmd_create), &resp_create, sizeof(resp_create)) ||
            resp_create.type != VIRTIO_GPU_RESP_OK_NODATA) {
            log_dec("VIRTIO-GPU: ERROR - RESOURCE_CREATE_2D failed for cursor shape ", shape);
            return false;
        }

        /* 64*64*4 = 16KB = 4 pages; translate each page on its own rather
           than assuming the kernel image is physically contiguous. */
        memset(&cursor_attach_hdr, 0, sizeof(cursor_attach_hdr));
        cursor_attach_hdr.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
        cursor_attach_hdr.resource_id = res_id;
        cursor_attach_hdr.num_entries = 4;
        for (uint32_t i = 0; i < 4; i++) {
            uint64_t va = (uint64_t)(uintptr_t)cursor_pixels[shape] + (uint64_t)i * 4096;
            cursor_shape_entries[shape][i].addr = vmm_get_phys(pml4, va);
            cursor_shape_entries[shape][i].length = 4096;
            cursor_shape_entries[shape][i].padding = 0;
        }

        if (!virtio_gpu_send_split_cmd(&cursor_attach_hdr, 32, cursor_shape_entries[shape],
                                       4 * sizeof(VirtioGpuMemEntry), &resp_attach, sizeof(resp_attach)) ||
            resp_attach.type != VIRTIO_GPU_RESP_OK_NODATA) {
            log_hex("VIRTIO-GPU: ERROR - RESOURCE_ATTACH_BACKING for cursor failed with type: ", resp_attach.type);
            return false;
        }

        memset(&cmd_transfer, 0, sizeof(cmd_transfer));
        cmd_transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
        cmd_transfer.resource_id = res_id;
        cmd_transfer.rww = CURSOR_DIM;
        cmd_transfer.rhh = CURSOR_DIM;

        if (!virtio_gpu_send_cmd((void *)&cmd_transfer, sizeof(cmd_transfer), &resp_transfer, sizeof(resp_transfer)) ||
            resp_transfer.type != VIRTIO_GPU_RESP_OK_NODATA) {
            log_str("VIRTIO-GPU: ERROR - TRANSFER_TO_HOST_2D for cursor failed!\r\n");
            return false;
        }
    }

    if (g_virgl) virtio_gpu_query_capsets();

    /* Show the arrow wherever the PS/2 driver already thinks the pointer is. */
    initialized = true;
    {
        extern int32_t mouse_get_x(void);
        extern int32_t mouse_get_y(void);
        uint64_t fl = irq_save();
        cursor_shape = 0;
        cursor_x = (uint32_t)mouse_get_x();
        cursor_y = (uint32_t)mouse_get_y();
        cursor_shape_dirty = true;
        cursor_submit();
        irq_restore(fl);
    }

    log_str("VIRTIO-GPU: Initialization and Hardware Cursor completed successfully!\r\n");
    return true;
}


/* Serializes framebuffer flushes: the TRANSFER/FLUSH command buffers
   below are shared statics, and a caller can be preempted mid-wait. */
static volatile int flush_busy = 0;

/* A flush whose completion hasn't been collected yet: one that timed
   out, or one submitted asynchronously by virtio_gpu_tick(). The static
   cmd_/resp_ buffers below still belong to the device until it completes,
   so no new flush is built while one is in flight. */
static bool       flush_inflight = false;
static bool       flush_timed_out = false; /* inflight because of a timeout */
static CtrlTicket flush_ticket;            /* the RESOURCE_FLUSH request */
static uint32_t   flush_stuck_drops = 0;

/* Set when a timed-out flush was collected from the tick (IRQ context,
   where we don't log); the next virtio_gpu_flush() reports it. */
static bool flush_resumed_unlogged = false;

/* Bounding box of every rect dropped (or timed out) while a flush was in
   flight: callers have already cleared their own damage, so it is re-sent
   by the next flush or, if none comes, by virtio_gpu_tick(). x2/y2
   exclusive. */
static bool     dropped_any = false;
static uint32_t dropped_x1, dropped_y1, dropped_x2, dropped_y2;

static void flush_remember_dropped(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!dropped_any) {
        dropped_x1 = x; dropped_y1 = y; dropped_x2 = x + w; dropped_y2 = y + h;
        dropped_any = true;
        return;
    }
    if (x < dropped_x1) dropped_x1 = x;
    if (y < dropped_y1) dropped_y1 = y;
    if (x + w > dropped_x2) dropped_x2 = x + w;
    if (y + h > dropped_y2) dropped_y2 = y + h;
}

/* Waiting for the host happens with interrupts OFF (syscall context), so
   the budget is kept short -- the timer, mouse IRQ and scheduler are all
   frozen for as long as it runs. A timeout is recoverable (flush_inflight),
   so there's no need to wait out a long host stall here. */
#define FLUSH_TIMEOUT_MS  20

/* Calibration window: TSC and timer_ms at its start. Rate = cycles per
   elapsed ms over >= TSC_CAL_WINDOW_MS, so late/bunched ticks cancel out.
   Ticks LOST during long interrupts-off stretches make a window read high,
   i.e. a longer timeout -- the safe direction; the smallest of the last
   few windows is used to shed those. */
#define TSC_CAL_WINDOW_MS 100
#define TSC_CAL_KEEP      4
static uint64_t tsc_win_start = 0, tsc_win_ms = 0;
static uint64_t tsc_win_rate[TSC_CAL_KEEP];
static uint32_t tsc_win_n = 0;

/* Collect an in-flight flush if the host has finished it. Returns false
   while it's still pending. flush_busy must be held. */
static bool flush_reclaim(void)
{
    if (!flush_inflight) return true;
    ctrlq_reap();
    if (!ctrlq_done(flush_ticket)) return false;
    flush_inflight = false;
    if (flush_timed_out) {
        flush_timed_out = false;
        flush_stuck_drops = 0;
        flush_resumed_unlogged = true;
    }
    return true;
}

/* Physical addresses of the static flush buffers, resolved once at init so
   flush_submit() never walks page tables from the timer IRQ on top of
   whatever process is current. */
static uint64_t phys_cmd_transfer, phys_resp_transfer, phys_cmd_flush, phys_resp_flush;

static void flush_resolve_phys(void)
{
    if (phys_cmd_transfer) return;
    PageTable *pml4 = vmm_get_current_pml4();
    phys_cmd_transfer  = vmm_get_phys(pml4, (uint64_t)(uintptr_t)&cmd_transfer);
    phys_resp_transfer = vmm_get_phys(pml4, (uint64_t)(uintptr_t)&resp_transfer);
    phys_cmd_flush     = vmm_get_phys(pml4, (uint64_t)(uintptr_t)&cmd_flush);
    phys_resp_flush    = vmm_get_phys(pml4, (uint64_t)(uintptr_t)&resp_flush);
}

/* Fold the dropped area into (x, y, w, h). */
static void flush_take_dropped(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
{
    if (!dropped_any) return;
    uint32_t x2 = *x + *w, y2 = *y + *h;
    if (dropped_x1 < *x) *x = dropped_x1;
    if (dropped_y1 < *y) *y = dropped_y1;
    if (dropped_x2 > x2) x2 = dropped_x2;
    if (dropped_y2 > y2) y2 = dropped_y2;
    *w = x2 - *x; *h = y2 - *y;
    dropped_any = false;
}

/* Queue TRANSFER_TO_HOST_2D + RESOURCE_FLUSH for the rect behind one
   notify each. The device runs its control queue in order for 2D
   commands, so the flush always sees the transfer, and the flush's
   completion implies the transfer's. flush_busy must be held and no flush
   may be in flight. */
static bool flush_submit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, CtrlTicket *flush_t)
{
    uint32_t scr_w = comp_get_width();

    memset(&cmd_transfer, 0, sizeof(cmd_transfer));
    cmd_transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd_transfer.resource_id = 1;
    cmd_transfer.rxx = x;
    cmd_transfer.ryy = y;
    cmd_transfer.rww = w;
    cmd_transfer.rhh = h;
    cmd_transfer.offset = ((uint64_t)y * scr_w + x) * 4;

    memset(&cmd_flush, 0, sizeof(cmd_flush));
    cmd_flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd_flush.resource_id = 1;
    cmd_flush.rxx = x;
    cmd_flush.ryy = y;
    cmd_flush.rww = w;
    cmd_flush.rhh = h;

    flush_resolve_phys();
    CtrlSeg tseg[2] = {
        { phys_cmd_transfer,  sizeof(cmd_transfer),  false },
        { phys_resp_transfer, sizeof(resp_transfer), true  },
    };
    CtrlSeg fseg[2] = {
        { phys_cmd_flush,  sizeof(cmd_flush),  false },
        { phys_resp_flush, sizeof(resp_flush), true  },
    };
    /* Both or neither: reserve room for four descriptors first. */
    ctrlq_reap();
    if (ctrl_q.num_free < 4) return false;
    CtrlTicket tt;
    if (!ctrlq_submit(tseg, 2, &tt)) return false;
    return ctrlq_submit(fseg, 2, flush_t);
}

void virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!initialized) return;

    /* Clip to the scanout: a bogus rect from a Ring-3 SYS_fb_flush caller
       would otherwise make the host reject the command (or copy junk). */
    uint32_t scr_w = comp_get_width();
    uint32_t scr_h = comp_get_height();
    if (x >= scr_w || y >= scr_h || w == 0 || h == 0) return;
    if (w > scr_w - x) w = scr_w - x;
    if (h > scr_h - y) h = scr_h - y;

    while (__sync_lock_test_and_set(&flush_busy, 1)) {
        extern void sched_yield(void);
        sched_yield();
    }

    /* An async flush from the tick is normally done within a few ms:
       wait it out (same budget as our own wait) rather than drop a frame.
       After a real timeout the host is known-stuck -- don't wait again. */
    if (flush_inflight && !flush_timed_out) {
        ctrlq_wait_spin(flush_ticket, FLUSH_TIMEOUT_MS);
    }
    if (!flush_reclaim()) {
        /* host still busy with an earlier flush: drop this frame, its
           area goes out with the next flush or tick */
        flush_remember_dropped(x, y, w, h);
        if (flush_timed_out && (++flush_stuck_drops % 1000) == 0) {
            log_dec("VIRTIO-GPU: host still not responding, dropped frames = ", flush_stuck_drops);
        }
        __sync_lock_release(&flush_busy);
        return;
    }

    if (flush_resumed_unlogged) {
        flush_resumed_unlogged = false;
        log_str("VIRTIO-GPU: stuck flush completed, resuming\r\n");
    }

    flush_take_dropped(&x, &y, &w, &h);

    CtrlTicket t;
    if (!flush_submit(x, y, w, h, &t)) {
        flush_remember_dropped(x, y, w, h);
        __sync_lock_release(&flush_busy);
        return;
    }

    if (!ctrlq_wait_spin(t, FLUSH_TIMEOUT_MS)) {
        log_str("VIRTIO-GPU: flush timeout!\r\n");
        flush_inflight = true;
        flush_timed_out = true;
        flush_ticket = t;
        /* The host may never apply this rect: send it again on recovery. */
        flush_remember_dropped(x, y, w, h);
    }

    __sync_lock_release(&flush_busy);
}

bool virtio_gpu_is_active(void)
{
    return initialized;
}

/* Called from the mouse IRQ on every pointer move. */
void virtio_gpu_move_cursor(uint32_t x, uint32_t y)
{
    if (!initialized) return;
    uint64_t fl = irq_save();
    cursor_x = x;
    cursor_y = y;
    cursor_submit();
    irq_restore(fl);
}

/* 0 arrow, 1 hand, 2 text, 3 resize. Switching shape is one UPDATE_CURSOR
   pointing at an already-uploaded resource -- no pixel transfer. */
void virtio_gpu_set_cursor_shape(uint32_t shape)
{
    if (!initialized || shape >= CURSOR_SHAPE_COUNT) return;
    uint64_t fl = irq_save();
    if (shape != cursor_shape) {
        cursor_shape = shape;
        cursor_shape_dirty = true;
        cursor_submit();
    }
    irq_restore(fl);
}

/* Timer IRQ hook (1 kHz):
   - calibrates the TSC against the 1 kHz clock over 100 ms windows
     (for virtio_gpu_flush()'s time-based timeout);
   - resends the latest cursor state if the cursor queue was full when
     the mouse IRQ tried to send it;
   - collects a finished in-flight flush and pushes out any area dropped
     meanwhile WITHOUT waiting for it, so the screen catches up even when
     nobody calls virtio_gpu_flush() again (an idle window's last frame). */
void virtio_gpu_tick(void)
{
    if (!initialized) return;

    {
        extern uint64_t timer_get_ms(void);
        uint64_t now = rdtsc(), ms = timer_get_ms();
        if (tsc_win_start == 0) {
            tsc_win_start = now; tsc_win_ms = ms;
        } else if (ms - tsc_win_ms >= TSC_CAL_WINDOW_MS) {
            tsc_win_rate[tsc_win_n % TSC_CAL_KEEP] = (now - tsc_win_start) / (ms - tsc_win_ms);
            tsc_win_n++;
            uint32_t have = tsc_win_n < TSC_CAL_KEEP ? tsc_win_n : TSC_CAL_KEEP;
            uint64_t best = tsc_win_rate[0];
            for (uint32_t i = 1; i < have; i++)
                if (tsc_win_rate[i] < best) best = tsc_win_rate[i];
            tsc_per_ms = best;
            tsc_win_start = now; tsc_win_ms = ms;
        }
    }

    if (cursor_pending) {
        uint64_t fl = irq_save();
        if (cursor_pending) cursor_submit();
        irq_restore(fl);
    }

    /* Retire finished control requests even when nobody is waiting on
       them (async 3D submissions, a flush the tick sent). */
    ctrlq_reap();

    if (!dropped_any) return;
    /* Only try-lock: a preempted flush holder must not be spun on here. */
    if (__sync_lock_test_and_set(&flush_busy, 1)) return;
    if (flush_reclaim() && dropped_any) {
        uint32_t x = dropped_x1, y = dropped_y1;
        uint32_t w = dropped_x2 - dropped_x1, h = dropped_y2 - dropped_y1;
        dropped_any = false;
        CtrlTicket t;
        if (flush_submit(x, y, w, h, &t)) {
            flush_inflight = true;
            flush_timed_out = false;
            flush_ticket = t;
        } else {
            flush_remember_dropped(x, y, w, h);
        }
    }
    __sync_lock_release(&flush_busy);
}

/* Legacy entry point: position (and shape via resource id) in one go. */
void virtio_gpu_update_cursor(uint32_t resource_id, uint32_t x, uint32_t y)
{
    if (!initialized) return;
    if (resource_id >= CURSOR_RES_BASE && resource_id < CURSOR_RES_BASE + CURSOR_SHAPE_COUNT) {
        virtio_gpu_set_cursor_shape(resource_id - CURSOR_RES_BASE);
    }
    virtio_gpu_move_cursor(x, y);
}
