/*
 * WynlandOS - AHCI SATA Driver Implementation
 */

#include <wynland/ahci.h>
#include <wynland/pci.h>
#include <wynland/vmm.h>
#include <wynland/heap.h>
#include <wynland/pmm.h>
#include <wynland/types.h>

extern void serial_write_string(const char *str);


static volatile HbaMem *hba_mem = NULL;
int sata_port_num = -1;

static uint8_t *bounce(void);

void ahci_init(void) {
    (void)bounce();
    serial_write_string("AHCI: Scanning PCI for SATA controller...\r\n");
    PciDevice dev;
    if (!pci_find_device(0x01, 0x06, &dev)) {
        serial_write_string("AHCI Error: No SATA AHCI controller found on PCI bus!\r\n");
        return;
    }
    
    serial_write_string("AHCI: Found SATA controller.\r\n");
    
    // Map the BAR5 MMIO space
    uint64_t bar5_addr = dev.bar5 & 0xFFFFFFF0; // Clear flag bits
    vmm_map_mmio(bar5_addr, sizeof(HbaMem));
    hba_mem = (volatile HbaMem *)(uintptr_t)bar5_addr;

    // Enable AHCI globally
    hba_mem->ghc |= (1U << 31); // Set AHCI Enable (AE)


    
    // Print controller version
    serial_write_string("AHCI: Controller version: ");
    char ver_buf[16];
    extern void uint_to_hex(uint64_t val, char *buf);
    uint_to_hex(hba_mem->vs, ver_buf);
    serial_write_string(ver_buf);
    serial_write_string("\r\n");
    
    // Find first active SATA port
    uint32_t pi = hba_mem->pi;
    for (int i = 0; i < 32; i++) {
        if (pi & (1 << i)) {
            volatile HbaPort *port = &hba_mem->ports[i];
            uint32_t ssts = port->ssts;
            uint8_t ipm = (ssts >> 8) & 0x0F;
            uint8_t det = ssts & 0x0F;
            
            if (det == 3 && ipm == 1 && port->sig == SATA_SIG_ATA) {
                serial_write_string("AHCI: Found SATA drive on port ");
                char port_buf[8];
                extern void uint_to_str(uint64_t val, char *buf);
                uint_to_str(i, port_buf);
                serial_write_string(port_buf);
                serial_write_string("\r\n");
                
                sata_port_num = i;
                break;
            }
        }
    }
    
    if (sata_port_num == -1) {
        serial_write_string("AHCI Error: No active SATA drive found!\r\n");
        return;
    }
    
    // Initialize the selected port
    volatile HbaPort *port = &hba_mem->ports[sata_port_num];
    
    // 1. Stop port command execution
    port->cmd &= ~0x0001; // ST = 0
    port->cmd &= ~0x0010; // FRE = 0
    while (port->cmd & 0x8000 || port->cmd & 0x4000); // Wait for CR and FR to clear
    
    // 2. Allocate and set Command List Base and FIS Base
    // Allocate 1 physical page (4096 bytes) for Command List (1024 bytes) and FIS (256 bytes)
    void *cl_phys = pmm_alloc_page();
    memset(cl_phys, 0, 4096);
    
    uint64_t cl_addr = (uint64_t)(uintptr_t)cl_phys;
    uint64_t fis_addr = cl_addr + 1024;
    uint64_t ct_addr = cl_addr + 2048; // Command table base
    
    port->clb = (uint32_t)cl_addr;
    port->clbu = (uint32_t)(cl_addr >> 32);
    
    port->fb = (uint32_t)fis_addr;
    port->fbu = (uint32_t)(fis_addr >> 32);
    
    // 3. Set command headers
    HbaCmdHeader *cmd_header = (HbaCmdHeader *)(uintptr_t)cl_phys;
    for (int i = 0; i < 32; i++) {
        cmd_header[i].flags0 = 5; // CFL = 5 dwords (20 bytes)
        cmd_header[i].flags1 = 0;
        cmd_header[i].prdtl = 8; // 8 PRDT entries
        uint64_t port_ct_addr = ct_addr + i * 256;
        cmd_header[i].ctba = (uint32_t)port_ct_addr;
        cmd_header[i].ctbau = (uint32_t)(port_ct_addr >> 32);
    }
    
    // 4. Start port command execution
    while (port->cmd & 0x8000); // Wait for CR to clear
    port->serr = 0xFFFFFFFF; // Clear SATA errors
    port->is = 0xFFFFFFFF;   // Clear pending interrupts
    port->cmd |= 0x0010; // FRE = 1
    port->cmd |= 0x0001; // ST = 1
    
    serial_write_string("AHCI: Port initialized successfully.\r\n");
}

/* ahci_read - read sectors from SATA disk via AHCI DMA */
/* After a Task File Error the port stays in an error state: every later
   command fails too. Stop the command engine, clear the error bits and
   start it again (AHCI 1.3, 6.2.2.1), and say what failed. */
static void ahci_recover(volatile HbaPort *port, const char *op, uint32_t lba, uint32_t count, uint64_t phys)
{
    extern void uint_to_hex(uint64_t val, char *buf);
    char b[32];
    serial_write_string("AHCI "); serial_write_string(op);
    serial_write_string(" error: lba="); uint_to_hex(lba, b); serial_write_string(b);
    serial_write_string(" count="); uint_to_hex(count, b); serial_write_string(b);
    serial_write_string(" phys="); uint_to_hex(phys, b); serial_write_string(b);
    serial_write_string(" tfd="); uint_to_hex(port->tfd, b); serial_write_string(b);
    serial_write_string(" serr="); uint_to_hex(port->serr, b); serial_write_string(b);
    serial_write_string("\r\n");
    port->cmd &= ~0x0001u;                      /* ST off */
    for (int i = 0; i < 1000000 && (port->cmd & 0x8000u); i++) { } /* CR clears */
    port->serr = 0xFFFFFFFF;
    port->is = 0xFFFFFFFF;
    port->cmd |= 0x0001u;                       /* ST on */
}

/* DMA goes to ONE physical range (a single PRDT entry, from the
   buffer's first byte). A kernel heap buffer is only virtually
   contiguous: one crossing a page boundary had the device write its
   second part into whatever frame follows the first -- someone else's
   memory, and the caller got garbage (block numbers far past the disk,
   loader crashes). Such buffers go through a physically contiguous
   bounce buffer instead. */
#define AHCI_BOUNCE_PAGES 128                     /* 512 KB: >= any one request */
static uint8_t *g_bounce;

static bool dma_contiguous(const void *buf, uint32_t len)
{
    PageTable *pml4 = vmm_get_current_pml4();
    uint64_t va = (uint64_t)(uintptr_t)buf;
    uint64_t p0 = pml4 ? vmm_get_phys(pml4, va) : va;
    if (!p0) return false;
    for (uint64_t off = PAGE_SIZE - (va & (PAGE_SIZE - 1)); off < len; off += PAGE_SIZE)
        if (vmm_get_phys(pml4, va + off) != p0 + off) return false;
    return true;
}

static uint8_t *bounce(void)
{
    if (!g_bounce) {
        extern void *pmm_alloc_contiguous(uint32_t count);
        g_bounce = (uint8_t *)pmm_alloc_contiguous(AHCI_BOUNCE_PAGES);  /* identity-mapped */
    }
    return g_bounce;
}

bool ahci_read(uint32_t lba, uint32_t count, void *buf) {
    /* Everything uses command slot 0: a second command started while one
       is in flight (a kernel thread preempted mid-command, then a syscall
       reading) corrupted both -- "Task File Error", or a library read
       back damaged. Interrupts stay off for the whole command. */
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    bool ok = true;
    if (dma_contiguous(buf, count * 512)) {
        ok = ahci_read_hw(lba, count, buf);
    } else {
        uint8_t *b = bounce();
        uint8_t *out = (uint8_t *)buf;
        const uint32_t per = AHCI_BOUNCE_PAGES * PAGE_SIZE / 512;
        for (uint32_t done = 0; ok && done < count; ) {
            uint32_t n = count - done < per ? count - done : per;
            ok = b && ahci_read_hw(lba + done, n, b);
            if (ok) memcpy(out + (uint64_t)done * 512, b, (uint64_t)n * 512);
            done += n;
        }
    }
    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
    return ok;
}

/* Hardware-level read */
bool ahci_read_hw(uint32_t lba, uint32_t count, void *buf) {

    if (sata_port_num == -1) return false;
    volatile HbaPort *port = &hba_mem->ports[sata_port_num];
    
    // Clear interrupt status
    port->is = 0xFFFFFFFF;
    
    int slot = 0; // Use slot 0 for simplicity
    
    int timeout = 100000000;
    while ((port->ci & (1 << slot)) || (port->tfd & (AHCI_DEV_BUSY | AHCI_DEV_DRQ))) {
        if (--timeout == 0) {
            serial_write_string("AHCI Read: Port is busy/hung!\r\n");
            return false;
        }
    }
    
    // Translate virtual buffer address to physical address for DMA
    uint64_t phys_buf = (uint64_t)(uintptr_t)buf;
    PageTable *pml4 = vmm_get_current_pml4();
    if (pml4) {
        uint64_t resolved = vmm_get_phys(pml4, (uint64_t)(uintptr_t)buf);
        if (resolved != 0) {
            phys_buf = resolved;
        }
    }

    HbaCmdHeader *cmd_headers = (HbaCmdHeader *)(uintptr_t)port->clb;
    HbaCmdHeader *hdr = &cmd_headers[slot];
    hdr->flags0 = 5; // CFL = 5 dwords, W = 0 (Read)
    hdr->flags1 = 0;
    hdr->prdtl = 1; // 1 PRDT entry
    
    HbaCmdTable *tbl = (HbaCmdTable *)(uintptr_t)hdr->ctba;
    memset(tbl, 0, sizeof(HbaCmdTable) + sizeof(HbaPrdtEntry));
    
    tbl->prdt_entry[0].dba = (uint32_t)phys_buf;
    tbl->prdt_entry[0].dbau = (uint32_t)(phys_buf >> 32);
    tbl->prdt_entry[0].dbc = ((count * 512) - 1) | (1U << 31); // Size - 1, bit 31 = IOC
    
    FisRegH2D *fis = (FisRegH2D *)(tbl->cfis);
    fis->fis_type = 0x27; // Register FIS - Host to Device
    fis->c = 1; // Command
    fis->command = 0x25; // READ DMA EXT
    
    fis->lba0 = (uint8_t)lba;
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->device = 1 << 6; // LBA mode
    
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = 0;
    fis->lba5 = 0;
    
    fis->countl = (uint8_t)count;
    fis->counth = (uint8_t)(count >> 8);
    
    port->ci = (1 << slot);
    
    timeout = 100000000;
    while (1) {
        if ((port->ci & (1 << slot)) == 0) {
            break;
        }
        if (port->is & (1 << 30)) { // Task File Error
            ahci_recover(port, "read", lba, count, phys_buf);
            return false;
        }
        if (--timeout == 0) {
            serial_write_string("AHCI Read: Timeout waiting for command completion!\r\n");
            serial_write_string("Debug info:\r\n");
            char buf[32];
            extern void uint_to_hex(uint64_t val, char *buf);
            
            serial_write_string("  port->ci:   "); uint_to_hex(port->ci, buf); serial_write_string(buf); serial_write_string("\r\n");
            serial_write_string("  port->tfd:  "); uint_to_hex(port->tfd, buf); serial_write_string(buf); serial_write_string("\r\n");
            serial_write_string("  port->ssts: "); uint_to_hex(port->ssts, buf); serial_write_string(buf); serial_write_string("\r\n");
            serial_write_string("  port->cmd:  "); uint_to_hex(port->cmd, buf); serial_write_string(buf); serial_write_string("\r\n");
            serial_write_string("  port->is:   "); uint_to_hex(port->is, buf); serial_write_string(buf); serial_write_string("\r\n");
            serial_write_string("  port->serr: "); uint_to_hex(port->serr, buf); serial_write_string(buf); serial_write_string("\r\n");
            
            return false;
        }
    }
    
    if (port->is & (1 << 30)) {
        ahci_recover(port, "read", lba, count, phys_buf);
        return false;
    }
    
    return true;
}

static bool ahci_write_hw(uint32_t lba, uint32_t count, const void *buf);

bool ahci_write(uint32_t lba, uint32_t count, const void *buf) {
    uint64_t rflags;   /* see ahci_read(): one command at a time, contiguous DMA */
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    bool ok = true;
    if (dma_contiguous(buf, count * 512)) {
        ok = ahci_write_hw(lba, count, buf);
    } else {
        uint8_t *b = bounce();
        const uint8_t *in = (const uint8_t *)buf;
        const uint32_t per = AHCI_BOUNCE_PAGES * PAGE_SIZE / 512;
        for (uint32_t done = 0; ok && done < count; ) {
            uint32_t n = count - done < per ? count - done : per;
            if (b) memcpy(b, in + (uint64_t)done * 512, (uint64_t)n * 512);
            ok = b && ahci_write_hw(lba + done, n, b);
            done += n;
        }
    }
    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
    return ok;
}

static bool ahci_write_hw(uint32_t lba, uint32_t count, const void *buf) {
    if (sata_port_num == -1) return false;
    volatile HbaPort *port = &hba_mem->ports[sata_port_num];
    
    // Clear interrupt status
    port->is = 0xFFFFFFFF;
    
    int slot = 0; // Use slot 0 for simplicity
    
    int timeout = 100000000;
    while ((port->ci & (1 << slot)) || (port->tfd & (AHCI_DEV_BUSY | AHCI_DEV_DRQ))) {
        if (--timeout == 0) {
            serial_write_string("AHCI Write: Port is busy/hung!\r\n");
            return false;
        }
    }
    
    // Translate virtual buffer address to physical address for DMA
    uint64_t phys_buf = (uint64_t)(uintptr_t)buf;
    PageTable *pml4 = vmm_get_current_pml4();
    if (pml4) {
        uint64_t resolved = vmm_get_phys(pml4, (uint64_t)(uintptr_t)buf);
        if (resolved != 0) {
            phys_buf = resolved;
        }
    }

    HbaCmdHeader *cmd_headers = (HbaCmdHeader *)(uintptr_t)port->clb;
    HbaCmdHeader *hdr = &cmd_headers[slot];
    hdr->flags0 = 5 | (1 << 6); // CFL = 5 dwords, W = 1 (Write)
    hdr->flags1 = 0;
    hdr->prdtl = 1; // 1 PRDT entry
    
    HbaCmdTable *tbl = (HbaCmdTable *)(uintptr_t)hdr->ctba;
    memset(tbl, 0, sizeof(HbaCmdTable) + sizeof(HbaPrdtEntry));
    
    tbl->prdt_entry[0].dba = (uint32_t)phys_buf;
    tbl->prdt_entry[0].dbau = (uint32_t)(phys_buf >> 32);
    tbl->prdt_entry[0].dbc = ((count * 512) - 1) | (1U << 31); // Size - 1, bit 31 = IOC
    
    FisRegH2D *fis = (FisRegH2D *)(tbl->cfis);
    fis->fis_type = 0x27; // Register FIS - Host to Device
    fis->c = 1; // Command
    fis->command = 0x35; // WRITE DMA EXT
    
    fis->lba0 = (uint8_t)lba;
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->device = 1 << 6; // LBA mode
    
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = 0;
    fis->lba5 = 0;
    
    fis->countl = (uint8_t)count;
    fis->counth = (uint8_t)(count >> 8);
    
    port->ci = (1 << slot);
    
    timeout = 100000000;
    while (1) {
        if ((port->ci & (1 << slot)) == 0) {
            break;
        }
        if (port->is & (1 << 30)) { // Task File Error
            serial_write_string("AHCI Write Error: Task File Error!\r\n");
            return false;
        }
        if (--timeout == 0) {
            serial_write_string("AHCI Write: Timeout waiting for command completion!\r\n");
            return false;
        }
    }
    
    if (port->is & (1 << 30)) {
        serial_write_string("AHCI Write Error: Task File Error!\r\n");
        return false;
    }
    
    return true;
}
