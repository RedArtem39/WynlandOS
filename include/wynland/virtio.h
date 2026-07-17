/*
 * WynlandOS - Virtio Definitions
 *
 * Virtio 0.9.5 (Legacy/Transitional) PCI transport definitions.
 * Used by the virtio-net driver to communicate with QEMU's virtio-net device.
 */
#pragma once

#include <wynland/types.h>

/* ============================================================
 * Virtio PCI Legacy I/O Port Offsets
 * ============================================================ */

#define VIRTIO_PCI_HOST_FEATURES    0x00  /* 4 bytes, R   - Features offered by device */
#define VIRTIO_PCI_GUEST_FEATURES   0x04  /* 4 bytes, R/W - Features activated by driver */
#define VIRTIO_PCI_QUEUE_PFN        0x08  /* 4 bytes, R/W - Queue physical page number */
#define VIRTIO_PCI_QUEUE_SIZE       0x0C  /* 2 bytes, R   - Queue size (entries) */
#define VIRTIO_PCI_QUEUE_SELECT     0x0E  /* 2 bytes, R/W - Select queue index */
#define VIRTIO_PCI_QUEUE_NOTIFY     0x10  /* 2 bytes, R/W - Notify device of queue activity */
#define VIRTIO_PCI_STATUS           0x12  /* 1 byte,  R/W - Device status */
#define VIRTIO_PCI_ISR              0x13  /* 1 byte,  R   - Interrupt status */
#define VIRTIO_PCI_CONFIG           0x14  /* Device-specific config starts here */

/* ============================================================
 * Device Status Bits
 * ============================================================ */

#define VIRTIO_STATUS_RESET         0
#define VIRTIO_STATUS_ACKNOWLEDGE   1
#define VIRTIO_STATUS_DRIVER        2
#define VIRTIO_STATUS_DRIVER_OK     4
#define VIRTIO_STATUS_FEATURES_OK   8
#define VIRTIO_STATUS_FAILED        128

/* ============================================================
 * Virtio-net Feature Bits
 * ============================================================ */

#define VIRTIO_NET_F_CSUM           (1U << 0)
#define VIRTIO_NET_F_MAC            (1U << 5)
#define VIRTIO_NET_F_STATUS         (1U << 16)
#define VIRTIO_NET_F_MRG_RXBUF     (1U << 15)

/* ============================================================
 * Virtqueue Descriptor Flags
 * ============================================================ */

#define VIRTQ_DESC_F_NEXT          1   /* Buffer continues via 'next' field */
#define VIRTQ_DESC_F_WRITE         2   /* Buffer is device-writable (for RX) */

/* ============================================================
 * Virtqueue Structures
 * ============================================================ */

/* Single descriptor in the descriptor table */
typedef struct {
    uint64_t addr;    /* Physical address of the buffer */
    uint32_t len;     /* Length of the buffer */
    uint16_t flags;   /* VIRTQ_DESC_F_* */
    uint16_t next;    /* Next descriptor index (if F_NEXT) */
} __attribute__((packed)) VirtqDesc;

/* Available ring: driver -> device */
typedef struct {
    uint16_t flags;
    uint16_t idx;       /* Next index the driver will write to */
    uint16_t ring[];    /* Array of descriptor chain heads */
} __attribute__((packed)) VirtqAvail;

/* Single element in the used ring */
typedef struct {
    uint32_t id;   /* Index of the descriptor chain head */
    uint32_t len;  /* Total bytes written by device */
} __attribute__((packed)) VirtqUsedElem;

/* Used ring: device -> driver */
typedef struct {
    uint16_t flags;
    uint16_t idx;          /* Next index the device will write to */
    VirtqUsedElem ring[];  /* Completed descriptor chains */
} __attribute__((packed)) VirtqUsed;

/* ============================================================
 * Queue size and packet header
 * ============================================================ */

#define VIRTQ_SIZE   256  /* Number of descriptors per queue */
#define VIRTIO_NET_HDR_SIZE 10

/* Virtio-net header prepended to every packet */
typedef struct {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed)) VirtioNetHdr;

/* ============================================================
 * Virtqueue management structure (driver-side)
 * ============================================================ */

typedef struct {
    VirtqDesc  *desc;      /* Descriptor table */
    VirtqAvail *avail;     /* Available ring */
    VirtqUsed  *used;      /* Used ring */
    uint16_t    size;      /* Number of descriptors */
    uint16_t    free_head; /* Head of free descriptor list */
    uint16_t    num_free;  /* Number of free descriptors */
    uint16_t    last_used; /* Last used index we processed */
} Virtqueue;
