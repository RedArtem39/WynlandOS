/*
 * WynlandOS - AHCI SATA Driver Header
 */
#pragma once

#include <wynland/types.h>

#define SATA_SIG_ATA    0x00000101  // SATA drive
#define SATA_SIG_ATAPI  0xEB140101  // SATAPI drive

#define AHCI_DEV_BUSY   0x80
#define AHCI_DEV_DRQ    0x08

typedef struct {
    uint32_t clb;       // Command list base address (lower 32-bit)
    uint32_t clbu;      // Command list base address (upper 32-bit)
    uint32_t fb;        // FIS base address (lower 32-bit)
    uint32_t fbu;       // FIS base address (upper 32-bit)
    uint32_t is;        // Interrupt status
    uint32_t ie;        // Interrupt enable
    uint32_t cmd;       // Command and status
    uint32_t rsv0;      // Reserved
    uint32_t tfd;       // Task file data
    uint32_t sig;       // Signature
    uint32_t ssts;      // SATA status
    uint32_t sctl;      // SATA control
    uint32_t serr;      // SATA error
    uint32_t sact;      // SATA active
    uint32_t ci;        // Command issue
    uint32_t sntf;      // SATA notification
    uint32_t fbs;       // FIS-based switching control
    uint32_t rsv1[11];  // Reserved
    uint32_t vendor[4]; // Vendor specific
} __attribute__((packed)) HbaPort;

typedef struct {
    uint32_t cap;       // Host capabilities
    uint32_t ghc;       // Global host control
    uint32_t is;        // Interrupt status
    uint32_t pi;        // Ports implemented
    uint32_t vs;        // Version
    uint32_t ccc_ctl;   // Command completion coalescing control
    uint32_t ccc_pts;   // Command completion coalescing ports
    uint32_t em_loc;    // Enclosure management location
    uint32_t em_ctl;    // Enclosure management control
    uint32_t cap2;      // Host capabilities 2
    uint32_t bohc;      // BIOS/OS handoff control and status
    uint8_t  rsv[116];  // Reserved
    uint8_t  vendor[96];// Vendor specific
    HbaPort  ports[32]; // up to 32 ports
} __attribute__((packed)) HbaMem;

typedef struct {
    uint8_t  flags0;    // bits 0-4: CFL, bit 5: A, bit 6: W, bit 7: P
    uint8_t  flags1;    // bit 0: R, bit 1: B, bit 2: C, bit 3: rsv0, bits 4-7: PMP
    uint16_t prdtl;     // Physical region descriptor table length
    volatile uint32_t prdbc; // Physical region descriptor byte count
    uint32_t ctba;      // Command table descriptor base address (lower 32-bit)
    uint32_t ctbau;     // Command table descriptor base address (upper 32-bit)
    uint32_t rsv1[4];   // Reserved
} __attribute__((packed)) HbaCmdHeader;

typedef struct {
    uint32_t dba;       // Data base address (lower 32-bit)
    uint32_t dbau;      // Data base address (upper 32-bit)
    uint32_t rsv0;      // Reserved
    uint32_t dbc;       // bits 0-21: byte count, bit 31: IOC
} __attribute__((packed)) HbaPrdtEntry;


typedef struct {
    uint8_t  cfis[64];  // Command FIS
    uint8_t  acmd[16];  // ATAPI command, 12 or 16 bytes
    uint8_t  rsv[48];   // Reserved
    HbaPrdtEntry prdt_entry[1]; // Physical Region Descriptor Table entries
} __attribute__((packed)) HbaCmdTable;

typedef struct {
    uint8_t  fis_type;
    uint8_t  pmport:4;
    uint8_t  rsv0:3;
    uint8_t  c:1;       // 1=Command, 0=Control
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  featureh;
    uint8_t  countl;
    uint8_t  counth;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  rsv1[4];
} __attribute__((packed)) FisRegH2D;

void ahci_init(void);
bool ahci_read(uint32_t lba, uint32_t count, void *buf);
bool ahci_read_hw(uint32_t lba, uint32_t count, void *buf);
bool ahci_write(uint32_t lba, uint32_t count, const void *buf);
