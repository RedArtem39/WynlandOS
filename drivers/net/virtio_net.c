/*
 * WynlandOS - Virtio-Net PCI Driver
 *
 * Implements virtio-net using legacy virtio 0.9.5 PCI transport.
 * Provides Ethernet frame send/receive over QEMU's virtio-net device.
 */

#include <wynland/types.h>
#include <wynland/virtio.h>
#include <wynland/net.h>
#include <wynland/pci.h>
#include <wynland/pmm.h>

/* ============================================================
 * External declarations (freestanding - no libc)
 * ============================================================ */

extern void  serial_write_string(const char *str);
extern void  uint_to_str(uint64_t value, char *buf);
extern void  uint_to_hex(uint64_t value, char *buf);
extern void *kmalloc(size_t size);

/* memset and memcpy are declared in types.h */

/* ============================================================
 * Port I/O intrinsics
 * ============================================================ */

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port));
}

/* ============================================================
 * Virtio-net PCI constants
 * ============================================================ */

#define VIRTIO_VENDOR_ID        0x1AF4
#define VIRTIO_NET_DEVICE_BASE  0x1000  /* Legacy transitional device */
#define VIRTIO_NET_SUBSYS_ID    1       /* Subsystem device ID for net */

#define PCI_CMD_BUS_MASTER      (1U << 2)

/* Queue indices */
#define RX_QUEUE    0
#define TX_QUEUE    1

/* Buffer sizes */
#define RX_BUF_SIZE     (VIRTIO_NET_HDR_SIZE + NET_PKT_BUF)
#define TX_BUF_SIZE     (VIRTIO_NET_HDR_SIZE + ETH_FRAME_MAX)

/* TX poll timeout (iterations) */
#define TX_POLL_TIMEOUT 1000000

/* ============================================================
 * Static driver state
 * ============================================================ */

/* I/O base address of the virtio-net device */
static uint16_t io_base;

/* MAC address */
static uint8_t mac_addr[ETH_ALEN];

/* Virtqueues for RX and TX */
static Virtqueue rxq;
static Virtqueue txq;

/* Static RX buffers - one per descriptor */
static uint8_t rx_buffers[VIRTQ_SIZE][RX_BUF_SIZE] __attribute__((aligned(16)));

/* Static TX buffers - one per descriptor */
static uint8_t tx_buffers[VIRTQ_SIZE][TX_BUF_SIZE] __attribute__((aligned(16)));

/* Track whether the device was initialized */
static bool initialized;

/* ============================================================
 * Debug logging helpers
 * ============================================================ */

static void log_str(const char *msg)
{
    serial_write_string(msg);
}

static void log_hex(const char *prefix, uint64_t val)
{
    char buf[20];
    serial_write_string(prefix);
    uint_to_hex(val, buf);
    serial_write_string(buf);
    serial_write_string("\r\n");
}

static void log_dec(const char *prefix, uint64_t val)
{
    char buf[24];
    serial_write_string(prefix);
    uint_to_str(val, buf);
    serial_write_string(buf);
    serial_write_string("\r\n");
}

/* ============================================================
 * PCI Discovery
 * ============================================================ */

/*
 * Find the virtio-net device on PCI bus.
 * Vendor 0x1AF4, device 0x1000 with subsystem device ID 1.
 * Returns the I/O base address from BAR0, or 0 on failure.
 */
static uint16_t virtio_pci_find(void)
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
                if (device_id != VIRTIO_NET_DEVICE_BASE)
                    continue;

                /* Check subsystem device ID (PCI config offset 0x2C, upper 16 bits) */
                uint32_t subsys_reg = pci_read_config((uint8_t)bus, slot, func, 0x2C);
                uint16_t subsys_dev = (subsys_reg >> 16) & 0xFFFF;

                if (subsys_dev != VIRTIO_NET_SUBSYS_ID)
                    continue;

                log_str("VIRTIO-NET: Found device on PCI ");
                log_dec("  bus=", bus);
                log_dec("  slot=", slot);
                log_dec("  func=", func);

                /* Read BAR0 for I/O base */
                uint32_t bar0 = pci_read_config((uint8_t)bus, slot, func, 0x10);
                uint16_t base = (uint16_t)(bar0 & ~0x3U); /* Mask lower 2 bits (I/O space) */

                log_hex("VIRTIO-NET: BAR0 I/O base = ", base);

                /* Enable bus mastering */
                uint32_t cmd = pci_read_config((uint8_t)bus, slot, func, 0x04);
                cmd |= PCI_CMD_BUS_MASTER;
                pci_write_config((uint8_t)bus, slot, func, 0x04, cmd);

                log_str("VIRTIO-NET: Bus mastering enabled\r\n");

                return base;
            }
        }
    }
    return 0;
}

/* ============================================================
 * Virtqueue Setup
 * ============================================================ */

/*
 * Calculate total bytes needed for a virtqueue of size q.
 *   desc table:  16 * q   (at offset 0)
 *   avail ring:  6 + 2*q  (at offset 16*q)
 *   used ring:   6 + 8*q  (at page-aligned offset)
 */
static bool virtqueue_setup(Virtqueue *vq, uint16_t queue_idx)
{
    /* Select the queue */
    outw(io_base + VIRTIO_PCI_QUEUE_SELECT, queue_idx);

    /* Read queue size */
    uint16_t qsz = inw(io_base + VIRTIO_PCI_QUEUE_SIZE);
    if (qsz == 0) {
        log_str("VIRTIO-NET: Queue size is 0, cannot setup\r\n");
        return false;
    }

    log_dec("VIRTIO-NET: Queue index = ", queue_idx);
    log_dec("VIRTIO-NET: Queue size  = ", qsz);

    vq->size = qsz;

    /* Calculate memory layout sizes */
    uint32_t desc_size  = 16 * (uint32_t)qsz;                     /* VirtqDesc is 16 bytes */
    uint32_t avail_size = 6 + 2 * (uint32_t)qsz;                  /* flags + idx + ring[qsz] */
    uint32_t avail_end  = desc_size + avail_size;
    uint32_t used_offset = (avail_end + 4095) & ~4095U;            /* Align to 4096 */
    uint32_t used_size  = 6 + 8 * (uint32_t)qsz;                  /* flags + idx + ring[qsz] */
    uint32_t total_size = used_offset + used_size;

    /* Allocate enough contiguous pages */
    uint32_t pages_needed = (total_size + 4095) / 4096;
    uint8_t *mem = (uint8_t *)pmm_alloc_page();
    if (!mem) {
        log_str("VIRTIO-NET: Failed to allocate first page for queue\r\n");
        return false;
    }

    /* Allocate remaining pages to ensure contiguous memory.
     * pmm_alloc_page() returns consecutive pages in WynlandOS's PMM. */
    for (uint32_t i = 1; i < pages_needed; i++) {
        void *extra = pmm_alloc_page();
        if (!extra) {
            log_str("VIRTIO-NET: Failed to allocate memory for queue\r\n");
            return false;
        }
        (void)extra;  /* Pages are contiguous, we just need to allocate them */
    }

    /* Zero all memory */
    memset(mem, 0, total_size);

    /* Set up pointers into the allocated memory */
    vq->desc  = (VirtqDesc *)mem;
    vq->avail = (VirtqAvail *)(mem + desc_size);
    vq->used  = (VirtqUsed *)(mem + used_offset);

    /* Initialize free descriptor chain */
    for (uint16_t i = 0; i < qsz; i++) {
        vq->desc[i].next = i + 1;
        vq->desc[i].flags = (i < (uint16_t)(qsz - 1)) ? VIRTQ_DESC_F_NEXT : 0;
    }
    vq->free_head = 0;
    vq->num_free  = qsz;
    vq->last_used = 0;

    /* Tell device the physical page number of this queue */
    uint32_t pfn = (uint32_t)((uintptr_t)mem / 4096);
    outl(io_base + VIRTIO_PCI_QUEUE_PFN, pfn);

    log_hex("VIRTIO-NET: Queue PFN = ", pfn);
    log_str("VIRTIO-NET: Queue setup complete\r\n");

    return true;
}

/* ============================================================
 * Descriptor management helpers
 * ============================================================ */

/*
 * Allocate a free descriptor from a virtqueue.
 * Returns the descriptor index, or 0xFFFF if none available.
 */
static uint16_t vq_alloc_desc(Virtqueue *vq)
{
    if (vq->num_free == 0)
        return 0xFFFF;

    uint16_t idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->num_free--;
    return idx;
}

/*
 * Return a descriptor to the free list.
 */
static void vq_free_desc(Virtqueue *vq, uint16_t idx)
{
    vq->desc[idx].next = vq->free_head;
    vq->desc[idx].flags = VIRTQ_DESC_F_NEXT;
    vq->free_head = idx;
    vq->num_free++;
}

/* ============================================================
 * RX Queue Initialization
 * ============================================================ */

/*
 * Pre-fill the RX queue with buffers so the device can write received packets.
 */
static void rx_queue_fill(void)
{
    for (uint16_t i = 0; i < rxq.size; i++) {
        uint16_t desc_idx = vq_alloc_desc(&rxq);
        if (desc_idx == 0xFFFF)
            break;

        /* Point descriptor at static RX buffer */
        rxq.desc[desc_idx].addr  = (uint64_t)(uintptr_t)rx_buffers[desc_idx];
        rxq.desc[desc_idx].len   = RX_BUF_SIZE;
        rxq.desc[desc_idx].flags = VIRTQ_DESC_F_WRITE;  /* Device writes into this buffer */
        rxq.desc[desc_idx].next  = 0;

        /* Add to available ring */
        uint16_t avail_idx = rxq.avail->idx % rxq.size;
        rxq.avail->ring[avail_idx] = desc_idx;
        rxq.avail->idx++;
    }

    /* Notify device that RX buffers are available */
    outw(io_base + VIRTIO_PCI_QUEUE_NOTIFY, RX_QUEUE);

    log_str("VIRTIO-NET: RX queue filled with buffers\r\n");
}

/* ============================================================
 * Driver Initialization
 * ============================================================ */

bool virtio_net_init(void)
{
    log_str("VIRTIO-NET: Initializing virtio-net driver...\r\n");

    /* Step 1: Find the device on PCI */
    io_base = virtio_pci_find();
    if (io_base == 0) {
        log_str("VIRTIO-NET: ERROR - Device not found on PCI bus!\r\n");
        return false;
    }

    /* Step 2: Reset device */
    outb(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_RESET);
    log_str("VIRTIO-NET: Device reset\r\n");

    /* Step 3: Set ACKNOWLEDGE status */
    outb(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    log_str("VIRTIO-NET: Status = ACKNOWLEDGE\r\n");

    /* Step 4: Set DRIVER status */
    outb(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    log_str("VIRTIO-NET: Status = DRIVER\r\n");

    /* Step 5: Negotiate features */
    uint32_t host_features = inl(io_base + VIRTIO_PCI_HOST_FEATURES);
    log_hex("VIRTIO-NET: Host features = ", host_features);

    uint32_t guest_features = 0;

    /* Accept MAC feature (bit 5) if offered */
    if (host_features & VIRTIO_NET_F_MAC) {
        guest_features |= VIRTIO_NET_F_MAC;
        log_str("VIRTIO-NET: Accepting VIRTIO_NET_F_MAC\r\n");
    }

    /* Do NOT accept MRG_RXBUF (bit 15) - use simple 10-byte header */
    if (host_features & VIRTIO_NET_F_MRG_RXBUF) {
        log_str("VIRTIO-NET: Rejecting VIRTIO_NET_F_MRG_RXBUF\r\n");
    }

    outl(io_base + VIRTIO_PCI_GUEST_FEATURES, guest_features);
    log_hex("VIRTIO-NET: Guest features = ", guest_features);

    /* Step 6: Set up RX queue (index 0) */
    if (!virtqueue_setup(&rxq, RX_QUEUE)) {
        log_str("VIRTIO-NET: ERROR - Failed to setup RX queue!\r\n");
        outb(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    /* Step 7: Set up TX queue (index 1) */
    if (!virtqueue_setup(&txq, TX_QUEUE)) {
        log_str("VIRTIO-NET: ERROR - Failed to setup TX queue!\r\n");
        outb(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    /* Step 8: Set DRIVER_OK - device is live */
    outb(io_base + VIRTIO_PCI_STATUS,
         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    log_str("VIRTIO-NET: Status = DRIVER_OK\r\n");

    /* Step 9: Read MAC address from device config space */
    for (int i = 0; i < ETH_ALEN; i++) {
        mac_addr[i] = inb(io_base + VIRTIO_PCI_CONFIG + i);
    }

    log_str("VIRTIO-NET: MAC address = ");
    for (int i = 0; i < ETH_ALEN; i++) {
        char hex[4];
        uint_to_hex(mac_addr[i], hex);
        /* uint_to_hex writes "0x.." prefix; print last 2 chars for compact MAC */
        serial_write_string(hex);
        if (i < ETH_ALEN - 1) serial_write_string(":");
    }
    serial_write_string("\r\n");

    /* Step 10: Fill RX queue with pre-allocated buffers */
    rx_queue_fill();

    initialized = true;
    log_str("VIRTIO-NET: Initialization complete!\r\n");
    return true;
}

/* ============================================================
 * Send Packet
 * ============================================================ */

bool virtio_net_send(const void *data, uint32_t len)
{
    if (!initialized)
        return false;

    if (len > ETH_FRAME_MAX)
        return false;

    /* Allocate a TX descriptor */
    uint16_t desc_idx = vq_alloc_desc(&txq);
    if (desc_idx == 0xFFFF) {
        log_str("VIRTIO-NET: TX queue full!\r\n");
        return false;
    }

    /* Build the TX buffer: VirtioNetHdr (10 bytes, zeroed) + packet data */
    uint8_t *buf = tx_buffers[desc_idx];
    memset(buf, 0, VIRTIO_NET_HDR_SIZE);             /* Zero the virtio-net header */
    memcpy(buf + VIRTIO_NET_HDR_SIZE, data, len);     /* Copy the packet data */

    /* Set up the descriptor */
    txq.desc[desc_idx].addr  = (uint64_t)(uintptr_t)buf;
    txq.desc[desc_idx].len   = VIRTIO_NET_HDR_SIZE + len;
    txq.desc[desc_idx].flags = 0;   /* No WRITE (device reads), no NEXT (single buffer) */
    txq.desc[desc_idx].next  = 0;

    /* Add to TX available ring */
    uint16_t avail_idx = txq.avail->idx % txq.size;
    txq.avail->ring[avail_idx] = desc_idx;

    /* Memory barrier before updating idx */
    __asm__ volatile("mfence" ::: "memory");

    txq.avail->idx++;

    /* Notify device of TX queue activity */
    outw(io_base + VIRTIO_PCI_QUEUE_NOTIFY, TX_QUEUE);

    /* Poll the used ring for completion with timeout */
    for (uint32_t i = 0; i < TX_POLL_TIMEOUT; i++) {
        if (txq.used->idx != txq.last_used) {
            txq.last_used++;
            /* Return the descriptor to the free list */
            vq_free_desc(&txq, desc_idx);
            return true;
        }
        __asm__ volatile("pause");
    }

    /* Timeout - return descriptor anyway to avoid leak */
    log_str("VIRTIO-NET: TX timeout!\r\n");
    vq_free_desc(&txq, desc_idx);
    return false;
}

/* ============================================================
 * Receive Packet
 * ============================================================ */

int virtio_net_receive(void *buf, uint32_t max_len)
{
    if (!initialized)
        return 0;

    /* Check if the device has placed anything in the used ring */
    if (rxq.used->idx == rxq.last_used)
        return 0;  /* No new packets */

    /* Get the used element */
    uint16_t used_idx = rxq.last_used % rxq.size;
    uint32_t desc_idx = rxq.used->ring[used_idx].id;
    uint32_t total_len = rxq.used->ring[used_idx].len;

    rxq.last_used++;

    /* The buffer contains VirtioNetHdr (10 bytes) followed by the actual packet */
    if (total_len <= VIRTIO_NET_HDR_SIZE) {
        /* No actual packet data - re-add buffer and return 0 */
        goto requeue;
    }

    uint32_t pkt_len = total_len - VIRTIO_NET_HDR_SIZE;
    if (pkt_len > max_len)
        pkt_len = max_len;

    /* Copy packet data (skip the virtio-net header) */
    uint8_t *rx_buf = rx_buffers[desc_idx];
    memcpy(buf, rx_buf + VIRTIO_NET_HDR_SIZE, pkt_len);

requeue:
    /* Re-add the descriptor to the RX available ring for reuse */
    rxq.desc[desc_idx].addr  = (uint64_t)(uintptr_t)rx_buffers[desc_idx];
    rxq.desc[desc_idx].len   = RX_BUF_SIZE;
    rxq.desc[desc_idx].flags = VIRTQ_DESC_F_WRITE;
    rxq.desc[desc_idx].next  = 0;

    uint16_t avail_idx = rxq.avail->idx % rxq.size;
    rxq.avail->ring[avail_idx] = (uint16_t)desc_idx;

    __asm__ volatile("mfence" ::: "memory");

    rxq.avail->idx++;

    /* Notify device that a buffer is available again */
    outw(io_base + VIRTIO_PCI_QUEUE_NOTIFY, RX_QUEUE);

    if (total_len <= VIRTIO_NET_HDR_SIZE)
        return 0;

    return (int)pkt_len;
}

/* ============================================================
 * Get MAC Address
 * ============================================================ */

void virtio_net_get_mac(uint8_t *mac)
{
    for (int i = 0; i < ETH_ALEN; i++) {
        mac[i] = mac_addr[i];
    }
}
