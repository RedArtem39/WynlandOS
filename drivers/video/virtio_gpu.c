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
static VirtioGpuMemEntry cursor_entries[4] __attribute__((aligned(4096)));
static VirtioGpuResourceAttachBacking fb_attach_hdr     __attribute__((aligned(4096)));
static VirtioGpuResourceAttachBacking cursor_attach_hdr __attribute__((aligned(4096)));
static VirtioGpuCtrlResponse resp_attach;

static VirtioGpuSetScanout            cmd_scanout;
static VirtioGpuCtrlResponse          resp_scanout;

static VirtioGpuTransferToHost2d cmd_transfer;
static VirtioGpuCtrlResponse          resp_transfer;

static VirtioGpuResourceFlush    cmd_flush;
static VirtioGpuCtrlResponse          resp_flush;

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
    uint8_t *mem = (uint8_t *)pmm_alloc_page();
    if (!mem) {
        return false;
    }

    for (uint32_t i = 1; i < pages_needed; i++) {
        pmm_alloc_page();
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
 * Command Submission
 * ============================================================ */

static bool virtio_gpu_send_split_cmd(void *cmd, uint32_t cmd_len, void *data, uint32_t data_len, void *resp, uint32_t resp_len)
{
    uint16_t desc1 = vq_alloc_desc(&ctrl_q);
    uint16_t desc2 = vq_alloc_desc(&ctrl_q);
    uint16_t desc3 = vq_alloc_desc(&ctrl_q);
    if (desc1 == 0xFFFF || desc2 == 0xFFFF || desc3 == 0xFFFF) {
        if (desc1 != 0xFFFF) vq_free_desc(&ctrl_q, desc1);
        if (desc2 != 0xFFFF) vq_free_desc(&ctrl_q, desc2);
        if (desc3 != 0xFFFF) vq_free_desc(&ctrl_q, desc3);
        return false;
    }

    PageTable *pml4 = vmm_get_current_pml4();
    uint64_t cmd_phys  = vmm_get_phys(pml4, (uint64_t)(uintptr_t)cmd);
    uint64_t data_phys = vmm_get_phys(pml4, (uint64_t)(uintptr_t)data);
    uint64_t resp_phys = vmm_get_phys(pml4, (uint64_t)(uintptr_t)resp);

    /* Descriptor 1: command header (32 bytes) */
    ctrl_q.desc[desc1].addr  = cmd_phys;
    ctrl_q.desc[desc1].len   = cmd_len;
    ctrl_q.desc[desc1].flags = VIRTQ_DESC_F_NEXT;
    ctrl_q.desc[desc1].next  = desc2;

    /* Descriptor 2: entries data (nr_entries * 16 bytes) */
    ctrl_q.desc[desc2].addr  = data_phys;
    ctrl_q.desc[desc2].len   = data_len;
    ctrl_q.desc[desc2].flags = VIRTQ_DESC_F_NEXT;
    ctrl_q.desc[desc2].next  = desc3;

    /* Descriptor 3: response (24 bytes) */
    ctrl_q.desc[desc3].addr  = resp_phys;
    ctrl_q.desc[desc3].len   = resp_len;
    ctrl_q.desc[desc3].flags = VIRTQ_DESC_F_WRITE;
    ctrl_q.desc[desc3].next  = 0;

    /* Add head of chain to available ring */
    uint16_t avail_idx = ctrl_q.avail->idx % ctrl_q.size;
    ctrl_q.avail->ring[avail_idx] = desc1;

    __asm__ volatile("mfence" ::: "memory");
    ctrl_q.avail->idx++;

    /* Notify control queue (queue index 0) */
    *ctrl_q_notify = CTRL_QUEUE;

    /* Poll for response */
    for (uint32_t i = 0; i < 100000000; i++) {
        if (ctrl_q.used->idx != ctrl_q.last_used) {
            ctrl_q.last_used++;
            vq_free_desc(&ctrl_q, desc1);
            vq_free_desc(&ctrl_q, desc2);
            vq_free_desc(&ctrl_q, desc3);
            return true;
        }
        __asm__ volatile("pause");
    }

    log_str("VIRTIO-GPU: Command execution timeout!\r\n");
    vq_free_desc(&ctrl_q, desc1);
    vq_free_desc(&ctrl_q, desc2);
    vq_free_desc(&ctrl_q, desc3);
    return false;
}

static bool virtio_gpu_send_cmd(void *cmd, uint32_t cmd_len, void *resp, uint32_t resp_len)
{
    uint16_t desc1 = vq_alloc_desc(&ctrl_q);
    uint16_t desc2 = vq_alloc_desc(&ctrl_q);
    if (desc1 == 0xFFFF || desc2 == 0xFFFF) {
        if (desc1 != 0xFFFF) vq_free_desc(&ctrl_q, desc1);
        if (desc2 != 0xFFFF) vq_free_desc(&ctrl_q, desc2);
        return false;
    }

    /* Translate virtual addresses to physical for DMA */
    PageTable *pml4 = vmm_get_current_pml4();
    uint64_t cmd_phys  = vmm_get_phys(pml4, (uint64_t)(uintptr_t)cmd);
    uint64_t resp_phys = vmm_get_phys(pml4, (uint64_t)(uintptr_t)resp);

    /* Out descriptor: command request */
    ctrl_q.desc[desc1].addr  = cmd_phys;
    ctrl_q.desc[desc1].len   = cmd_len;
    ctrl_q.desc[desc1].flags = VIRTQ_DESC_F_NEXT;
    ctrl_q.desc[desc1].next  = desc2;

    /* In descriptor: device response */
    ctrl_q.desc[desc2].addr  = resp_phys;
    ctrl_q.desc[desc2].len   = resp_len;
    ctrl_q.desc[desc2].flags = VIRTQ_DESC_F_WRITE;
    ctrl_q.desc[desc2].next  = 0;

    /* Add head of chain to available ring */
    uint16_t avail_idx = ctrl_q.avail->idx % ctrl_q.size;
    ctrl_q.avail->ring[avail_idx] = desc1;

    __asm__ volatile("mfence" ::: "memory");
    ctrl_q.avail->idx++;

    /* Notify control queue (queue index 0) */
    *ctrl_q_notify = CTRL_QUEUE;

    /* Poll for response */
    for (uint32_t i = 0; i < 100000000; i++) {
        if (ctrl_q.used->idx != ctrl_q.last_used) {
            ctrl_q.last_used++;
            vq_free_desc(&ctrl_q, desc1);
            vq_free_desc(&ctrl_q, desc2);
            return true;
        }
        __asm__ volatile("pause");
    }

    log_str("VIRTIO-GPU: Command execution timeout!\r\n");
    vq_free_desc(&ctrl_q, desc1);
    vq_free_desc(&ctrl_q, desc2);
    return false;
}





static uint32_t cursor_pixels[64 * 64] __attribute__((aligned(4096)));

/* Beautiful hardware cursor shape: classic arrow with white fill and black outline */
static const uint8_t default_hw_cursor[32][32] = {
    { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 1, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 1, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 1, 0, 0, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 0, 0, 0, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

/* ============================================================
 * Public Driver Interface
 * ============================================================ */

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

    /* Write accepted features back */
    mmio_write32(VIRTIO_MODERN_DRV_FEATURE_SEL, 0);
    mmio_write32(VIRTIO_MODERN_DRV_FEATURE, features0);

    mmio_write32(VIRTIO_MODERN_DRV_FEATURE_SEL, 1);
    mmio_write32(VIRTIO_MODERN_DRV_FEATURE, features1 | 0x01); /* Accept VIRTIO_F_VERSION_1 */

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

    /* Initialize Hardware Cursor Resource (Resource 2, Size 64x64) */
    memset(cursor_pixels, 0, sizeof(cursor_pixels));
    for (int cy_mask = 0; cy_mask < 32; cy_mask++) {
        for (int cx_mask = 0; cx_mask < 32; cx_mask++) {
            uint8_t t = default_hw_cursor[cy_mask][cx_mask];
            if (t == 1) {
                cursor_pixels[cy_mask * 64 + cx_mask] = 0xFF000000; /* Black outline */
            } else if (t == 2) {
                cursor_pixels[cy_mask * 64 + cx_mask] = 0xFFFFFFFF; /* White fill */
            }
        }
    }

    /* Reuse cmd_create, cmd_attach, cmd_transfer statically */
    memset(&cmd_create, 0, sizeof(cmd_create));
    cmd_create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd_create.resource_id = 2;
    cmd_create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    cmd_create.width = 64;
    cmd_create.height = 64;

    if (!virtio_gpu_send_cmd(&cmd_create, sizeof(cmd_create), &resp_create, sizeof(resp_create)) ||
        resp_create.type != VIRTIO_GPU_RESP_OK_NODATA) {
        log_str("VIRTIO-GPU: ERROR - RESOURCE_CREATE_2D for Cursor failed!\r\n");
        return false;
    }

    uint32_t cursor_page_count = 4;
    /* Use global cursor_attach_hdr */
    memset(&cursor_attach_hdr, 0, sizeof(cursor_attach_hdr));
    cursor_attach_hdr.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cursor_attach_hdr.resource_id = 2;
    cursor_attach_hdr.num_entries = cursor_page_count;

    uint64_t cursor_phys_addr = vmm_get_phys(pml4, (uint64_t)(uintptr_t)cursor_pixels);
    for (uint32_t i = 0; i < cursor_page_count; i++) {
        cursor_entries[i].addr = cursor_phys_addr + (uint64_t)i * 4096;
        cursor_entries[i].length = 4096;
        cursor_entries[i].padding = 0;
    }

    log_hex("VIRTIO-GPU: Cursor first physical page = ", cursor_phys_addr);

    log_str("VIRTIO-GPU: Debug Cursor Attach Command Bytes:\r\n");
    uint32_t *c_ptr = (uint32_t *)&cursor_attach_hdr;
    for (int idx = 0; idx < 8; idx++) {
        log_hex("  Word ", c_ptr[idx]);
    }

    uint32_t cursor_data_len = cursor_page_count * sizeof(VirtioGpuMemEntry);
    if (!virtio_gpu_send_split_cmd(&cursor_attach_hdr, 32, cursor_entries, cursor_data_len, &resp_attach, sizeof(resp_attach)) ||
        resp_attach.type != VIRTIO_GPU_RESP_OK_NODATA) {
        log_hex("VIRTIO-GPU: ERROR - RESOURCE_ATTACH_BACKING for Cursor failed with type: ", resp_attach.type);
        return false;
    }

    memset(&cmd_transfer, 0, sizeof(cmd_transfer));
    cmd_transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd_transfer.resource_id = 2;
    cmd_transfer.rxx = 0;
    cmd_transfer.ryy = 0;
    cmd_transfer.rww = 64;
    cmd_transfer.rhh = 64;
    cmd_transfer.offset = 0;

    if (!virtio_gpu_send_cmd((void *)&cmd_transfer, sizeof(cmd_transfer), &resp_transfer, sizeof(resp_transfer)) ||
        resp_transfer.type != VIRTIO_GPU_RESP_OK_NODATA) {
        log_str("VIRTIO-GPU: ERROR - TRANSFER_TO_HOST_2D for Cursor failed!\r\n");
        return false;
    }

    /* Move cursor initially off-screen or top-left */
    initialized = true;
    virtio_gpu_update_cursor(2, 0, 0);

    log_str("VIRTIO-GPU: Initialization and Hardware Cursor completed successfully!\r\n");
    return true;
}


void virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!initialized) return;

    /* 1. Transfer dirty rect back-buffer contents to Host Resource 1 */
    uint32_t scr_w = comp_get_width();
    memset(&cmd_transfer, 0, sizeof(cmd_transfer));
    cmd_transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd_transfer.resource_id = 1;
    cmd_transfer.rxx = x;
    cmd_transfer.ryy = y;
    cmd_transfer.rww = w;
    cmd_transfer.rhh = h;
    cmd_transfer.offset = (uint64_t)(y * scr_w + x) * 4;

    /* 2. Flush updated host VRAM resource contents onto host scanout display window */
    memset(&cmd_flush, 0, sizeof(cmd_flush));
    cmd_flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd_flush.resource_id = 1;
    cmd_flush.rxx = x;
    cmd_flush.ryy = y;
    cmd_flush.rww = w;
    cmd_flush.rhh = h;

    virtio_gpu_send_cmd((void *)&cmd_transfer, sizeof(cmd_transfer), &resp_transfer, sizeof(resp_transfer));
    virtio_gpu_send_cmd((void *)&cmd_flush, sizeof(cmd_flush), &resp_flush, sizeof(resp_flush));
}

bool virtio_gpu_is_active(void)
{
    return initialized;
}

static VirtioGpuUpdateCursor cmd_cursor;

void virtio_gpu_update_cursor(uint32_t resource_id, uint32_t x, uint32_t y)
{
    if (!initialized) return;

    /* Reclaim completed descriptors lazily */
    while (cursor_q.used->idx != cursor_q.last_used) {
        uint16_t used_idx = cursor_q.last_used % cursor_q.size;
        uint32_t desc_idx = cursor_q.used->ring[used_idx].id;
        vq_free_desc(&cursor_q, (uint16_t)desc_idx);
        cursor_q.last_used++;
    }

    memset(&cmd_cursor, 0, sizeof(cmd_cursor));
    cmd_cursor.hdr.type = VIRTIO_GPU_CMD_UPDATE_CURSOR;
    cmd_cursor.pos.x = x;
    cmd_cursor.pos.y = y;
    cmd_cursor.resource_id = resource_id;
    cmd_cursor.hot_x = 0;
    cmd_cursor.hot_y = 0;

    uint16_t desc = vq_alloc_desc(&cursor_q);
    if (desc == 0xFFFF) return;

    /* Translate virtual address to physical for DMA */
    PageTable *pml4 = vmm_get_current_pml4();
    cursor_q.desc[desc].addr  = vmm_get_phys(pml4, (uint64_t)(uintptr_t)&cmd_cursor);
    cursor_q.desc[desc].len   = sizeof(cmd_cursor);
    cursor_q.desc[desc].flags = 0;
    cursor_q.desc[desc].next  = 0;

    uint16_t avail_idx = cursor_q.avail->idx % cursor_q.size;
    cursor_q.avail->ring[avail_idx] = desc;

    __asm__ volatile("mfence" ::: "memory");
    cursor_q.avail->idx++;

    /* Notify cursor queue (queue index 1) */
    *cursor_q_notify = CURSOR_QUEUE;
}
