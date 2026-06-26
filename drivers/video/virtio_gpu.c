/*
 * WynlandOS - Virtio-GPU Display Driver Implementation
 * ============================================================
 * Legacy/Transitional PCI driver for Virtio-GPU.
 * Communicates with QEMU's virtio-gpu-pci device using Port I/O.
 */

#include <wynland/virtio_gpu.h>
#include <wynland/pci.h>
#include <wynland/pmm.h>

extern void serial_write_string(const char *str);
extern void uint_to_str(uint64_t val, char *buf);
extern void uint_to_hex(uint64_t val, char *buf);
extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);

extern uint32_t *comp_get_backbuffer(void);
extern uint32_t  comp_get_width(void);
extern uint32_t  comp_get_height(void);

/* PCI IDs for VirtIO */
#define VIRTIO_VENDOR_ID         0x1AF4
#define VIRTIO_GPU_DEVICE_LEGACY 0x1050  /* Transitional/Legacy GPU ID (0x1050) */

/* PCI command register bits */
#define PCI_CMD_BUS_MASTER       0x04

/* Control and Cursor Queues */
#define CTRL_QUEUE   0
#define CURSOR_QUEUE 1

static uint16_t io_base = 0;
static bool initialized = false;

static Virtqueue ctrl_q;
static Virtqueue cursor_q;

/* Static commands/responses in contiguous kernel memory */
static VirtioGpuResourceCreate2d      cmd_create;
static VirtioGpuCtrlResponse          resp_create;

static VirtioGpuResourceAttachBacking cmd_attach;
static VirtioGpuCtrlResponse          resp_attach;

static VirtioGpuSetScanout            cmd_scanout;
static VirtioGpuCtrlResponse          resp_scanout;

static VirtioGpuTransferToHost2d      cmd_transfer;
static VirtioGpuCtrlResponse          resp_transfer;

static VirtioGpuResourceFlush         cmd_flush;
static VirtioGpuCtrlResponse          resp_flush;

/* Port I/O Helpers */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
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

static uint16_t virtio_gpu_pci_find(void)
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

                /* Read BAR0 for Port I/O address */
                uint32_t bar0 = pci_read_config((uint8_t)bus, slot, func, 0x10);
                uint16_t base = (uint16_t)(bar0 & ~0x3U);

                log_hex("VIRTIO-GPU: BAR0 Port I/O Base = ", base);

                /* Enable PCI Bus Mastering */
                uint32_t cmd = pci_read_config((uint8_t)bus, slot, func, 0x04);
                cmd |= PCI_CMD_BUS_MASTER;
                pci_write_config((uint8_t)bus, slot, func, 0x04, cmd);

                log_str("VIRTIO-GPU: Bus mastering enabled\r\n");
                return base;
            }
        }
    }
    return 0;
}

/* ============================================================
 * Virtqueue Setup
 * ============================================================ */

static bool virtqueue_setup(Virtqueue *vq, uint16_t queue_idx)
{
    outw(io_base + VIRTIO_PCI_QUEUE_SELECT, queue_idx);

    uint16_t qsz = inw(io_base + VIRTIO_PCI_QUEUE_SIZE);
    if (qsz == 0) {
        log_str("VIRTIO-GPU: Queue size is 0!\r\n");
        return false;
    }

    vq->size = qsz;
    log_dec("VIRTIO-GPU: Setup Queue index = ", queue_idx);
    log_dec("VIRTIO-GPU: Queue size = ", qsz);

    uint32_t desc_size  = 16 * (uint32_t)qsz;
    uint32_t avail_size = 6 + 2 * (uint32_t)qsz;
    uint32_t avail_end  = desc_size + avail_size;
    uint32_t used_offset = (avail_end + 4095) & ~4095U;
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

    memset(mem, 0, total_size);

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

    uint32_t pfn = (uint32_t)((uintptr_t)mem / 4096);
    outl(io_base + VIRTIO_PCI_QUEUE_PFN, pfn);

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

static bool virtio_gpu_send_cmd(void *cmd, uint32_t cmd_len, void *resp, uint32_t resp_len)
{
    uint16_t desc1 = vq_alloc_desc(&ctrl_q);
    uint16_t desc2 = vq_alloc_desc(&ctrl_q);
    if (desc1 == 0xFFFF || desc2 == 0xFFFF) {
        if (desc1 != 0xFFFF) vq_free_desc(&ctrl_q, desc1);
        if (desc2 != 0xFFFF) vq_free_desc(&ctrl_q, desc2);
        return false;
    }

    /* Out descriptor: command request */
    ctrl_q.desc[desc1].addr  = (uint64_t)(uintptr_t)cmd;
    ctrl_q.desc[desc1].len   = cmd_len;
    ctrl_q.desc[desc1].flags = VIRTQ_DESC_F_NEXT;
    ctrl_q.desc[desc1].next  = desc2;

    /* In descriptor: device response */
    ctrl_q.desc[desc2].addr  = (uint64_t)(uintptr_t)resp;
    ctrl_q.desc[desc2].len   = resp_len;
    ctrl_q.desc[desc2].flags = VIRTQ_DESC_F_WRITE;
    ctrl_q.desc[desc2].next  = 0;

    /* Add head of chain to available ring */
    uint16_t avail_idx = ctrl_q.avail->idx % ctrl_q.size;
    ctrl_q.avail->ring[avail_idx] = desc1;

    __asm__ volatile("mfence" ::: "memory");
    ctrl_q.avail->idx++;

    /* Notify control queue (queue index 0) */
    outw(io_base + VIRTIO_PCI_QUEUE_NOTIFY, CTRL_QUEUE);

    /* Poll for response */
    for (uint32_t i = 0; i < 2000000; i++) {
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

/* ============================================================
 * Public Driver Interface
 * ============================================================ */

bool virtio_gpu_init(void)
{
    log_str("VIRTIO-GPU: Initializing VirtIO-GPU driver...\r\n");

    io_base = virtio_gpu_pci_find();
    if (io_base == 0) {
        log_str("VIRTIO-GPU: Legacy transitional VirtIO-GPU device not found on PCI bus.\r\n");
        return false;
    }

    /* 1. Reset Device */
    outb(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_RESET);

    /* 2. Set ACKNOWLEDGE and DRIVER */
    outb(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    outb(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* 3. Setup queues */
    if (!virtqueue_setup(&ctrl_q, CTRL_QUEUE)) {
        log_str("VIRTIO-GPU: ERROR - Failed to setup Control Queue!\r\n");
        outb(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    if (!virtqueue_setup(&cursor_q, CURSOR_QUEUE)) {
        log_str("VIRTIO-GPU: ERROR - Failed to setup Cursor Queue!\r\n");
        outb(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    /* 4. Set DRIVER_OK */
    outb(io_base + VIRTIO_PCI_STATUS,
         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    log_str("VIRTIO-GPU: Virtqueues initialized, status set to DRIVER_OK\r\n");

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
    memset(&cmd_attach, 0, sizeof(cmd_attach));
    cmd_attach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd_attach.resource_id = 1;
    cmd_attach.num_entries = 1;
    cmd_attach.entries[0].addr = (uint64_t)(uintptr_t)back_buffer;
    cmd_attach.entries[0].length = w * h * 4;

    if (!virtio_gpu_send_cmd(&cmd_attach, sizeof(cmd_attach), &resp_attach, sizeof(resp_attach)) ||
        resp_attach.type != VIRTIO_GPU_RESP_OK_NODATA) {
        log_str("VIRTIO-GPU: ERROR - RESOURCE_ATTACH_BACKING failed!\r\n");
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

    initialized = true;
    log_str("VIRTIO-GPU: Initialization completed successfully!\r\n");
    return true;
}

void virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!initialized) return;

    /* 1. Transfer dirty rect back-buffer contents to Host Resource 1 */
    memset(&cmd_transfer, 0, sizeof(cmd_transfer));
    cmd_transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd_transfer.resource_id = 1;
    cmd_transfer.rxx = x;
    cmd_transfer.ryy = y;
    cmd_transfer.rww = w;
    cmd_transfer.rhh = h;
    cmd_transfer.offset = 0;

    virtio_gpu_send_cmd(&cmd_transfer, sizeof(cmd_transfer), &resp_transfer, sizeof(resp_transfer));

    /* 2. Flush updated host VRAM resource contents onto host scanout display window */
    memset(&cmd_flush, 0, sizeof(cmd_flush));
    cmd_flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd_flush.resource_id = 1;
    cmd_flush.rxx = x;
    cmd_flush.ryy = y;
    cmd_flush.rww = w;
    cmd_flush.rhh = h;

    virtio_gpu_send_cmd(&cmd_flush, sizeof(cmd_flush), &resp_flush, sizeof(resp_flush));
}

bool virtio_gpu_is_active(void)
{
    return initialized;
}
