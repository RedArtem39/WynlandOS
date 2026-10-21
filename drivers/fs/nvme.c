/*
 * WynlandOS - NVMe (NVM Express) Driver Implementation
 *
 * Polling driver, no interrupts: one command at a time, like the AHCI
 * driver. Uses one admin queue pair and one I/O queue pair, PRP lists
 * for transfers larger than a page, and a bounce buffer for every
 * data transfer (the filesystem passes pages that need not be
 * physically contiguous).
 */

#include <wynland/nvme.h>
#include <wynland/ahci.h>
#include <wynland/pci.h>
#include <wynland/vmm.h>
#include <wynland/pmm.h>
#include <wynland/memlayout.h>
#include <wynland/types.h>
#include <wynland/disklabel.h>

extern void serial_write_string(const char *str);
extern void uint_to_hex(uint64_t val, char *buf);

#define NVME_ADMIN_Q_SIZE   64u
#define NVME_IO_Q_SIZE      64u
#define NVME_BOUNCE_PAGES   128u             /* 512 KB */
#define NVME_MAX_SECTORS    (NVME_BOUNCE_PAGES * (PAGE_SIZE / 512u))
#define NVME_DBS_OFFSET     0x1000u

/* Controller registers (BAR0), offsets per NVMe 1.4 */
typedef struct {
    volatile uint64_t cap;       /* 0x00 */
    volatile uint32_t vs;        /* 0x08 */
    volatile uint32_t intms;     /* 0x0C */
    volatile uint32_t intmc;     /* 0x10 */
    volatile uint32_t cc;        /* 0x14 */
    volatile uint32_t reserved0; /* 0x18 */
    volatile uint32_t csts;      /* 0x1C */
    volatile uint32_t nssr;      /* 0x20 */
    volatile uint32_t aqa;       /* 0x24 */
    volatile uint32_t asq_lo;    /* 0x28 */
    volatile uint32_t asq_hi;    /* 0x2C */
    volatile uint32_t acq_lo;    /* 0x30 */
    volatile uint32_t acq_hi;    /* 0x34 */
    volatile uint32_t cmbloc;    /* 0x38 */
    volatile uint32_t cmbsz;     /* 0x3C */
    uint8_t reserved[NVME_DBS_OFFSET - 0x40];
} __attribute__((packed)) NvmeRegs;

/* Submission queue entry (64 bytes) */
typedef struct {
    uint32_t dw0;        /* opcode:7:0 fuse:9:8 psdt:11:10 cid:31:16 */
    uint32_t nsid;
    uint32_t cdw2;
    uint32_t cdw3;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) NvmeSqe;

/* Completion queue entry (16 bytes) */
typedef struct {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;     /* bit 0: phase, bits 15:1: status code */
} __attribute__((packed)) NvmeCqe;

/* Admin opcodes */
#define NVME_ADMIN_CREATE_SQ    0x01
#define NVME_ADMIN_CREATE_CQ    0x05
#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_ADMIN_SET_FEATURES 0x09

/* I/O opcodes */
#define NVME_CMD_FLUSH          0x00
#define NVME_CMD_WRITE          0x01
#define NVME_CMD_READ           0x02

/* Identify CNS values */
#define NVME_CNS_NAMESPACE      0x00

/* Set Features FID */
#define NVME_FEAT_NUM_QUEUES    0x07

#define NVME_CSTS_RDY           (1u << 0)
#define NVME_CSTS_CFS           (1u << 1)
#define NVME_CC_EN              (1u << 0)
#define NVME_SQES_SHIFT         16
#define NVME_SQES_64B           6u
#define NVME_CQES_SHIFT         20
#define NVME_CQES_16B           4u

static volatile NvmeRegs *g_regs;
static NvmeSqe *g_asq, *g_iosq;
static NvmeCqe *g_acq, *g_iocq;
static uint64_t g_asq_phys, g_iosq_phys, g_acq_phys, g_iocq_phys;
static uint64_t *g_prp_list;          /* one page of PRP list entries */
static uint64_t g_prp_list_phys;
static uint8_t *g_id;                 /* one page: identify payload */
static uint64_t g_id_phys;
static uint8_t *g_bounce;
static uint64_t g_bounce_phys;
static uint16_t g_asq_tail, g_acq_head, g_iosq_tail, g_iocq_head;
static uint8_t g_acq_phase = 1, g_iocq_phase = 1;
static uint32_t g_db_stride = 4;
static uint32_t g_max_sectors = NVME_MAX_SECTORS;
static uint64_t g_ns_sectors;
static uint16_t g_cid;
static bool g_ok;

static inline void nvme_doorbell(uint16_t qid, bool cq, uint16_t value)
{
    uint32_t off = NVME_DBS_OFFSET + (2u * qid + (cq ? 1u : 0u)) * g_db_stride;
    *(volatile uint32_t *)((uint8_t *)g_regs + off) = value;
}

static void nvme_log_err(const char *what, uint32_t code)
{
    char buf[20];
    serial_write_string("NVMe Error: ");
    serial_write_string(what);
    serial_write_string(" status=0x");
    uint_to_hex(code, buf);
    serial_write_string(buf);
    serial_write_string("\r\n");
}

#define NVME_WAIT_CYCLES    5000000000ULL   /* ~2 s */
#define NVME_CMD_CYCLES     30000000000ULL  /* ~10 s */

static bool nvme_wait_csts(uint32_t mask, uint32_t want)
{
    uint64_t start = __builtin_ia32_rdtsc();
    for (;;) {
        uint32_t csts = g_regs->csts;
        if ((csts & mask) == want) return true;
        if (csts & NVME_CSTS_CFS) return false;
        if (__builtin_ia32_rdtsc() - start > NVME_WAIT_CYCLES) return false;
        __asm__ volatile("pause");
    }
}

/* Wait for a completion, consume it, and return the NVMe status code
   (0 = success, 0xFFFFFFFF = timeout). */
static uint32_t nvme_poll(NvmeCqe *cq, uint16_t qsize, uint16_t qid,
                          uint16_t *head, uint8_t *phase, uint32_t *result)
{
    uint64_t start = __builtin_ia32_rdtsc();
    for (;;) {
        volatile NvmeCqe *e = &cq[*head];
        uint16_t st = e->status;
        if ((st & 1u) == *phase) {
            __sync_synchronize();
            if (result) *result = e->result;
            uint32_t code = (uint32_t)(st >> 1) & 0x7FFFu;
            *head = (uint16_t)((*head + 1u) % qsize);
            if (*head == 0) *phase ^= 1u;
            nvme_doorbell(qid, true, *head);
            return code;
        }
        if (__builtin_ia32_rdtsc() - start > NVME_CMD_CYCLES)
            return 0xFFFFFFFFu;
        __asm__ volatile("pause");
    }
}

static uint32_t nvme_admin(uint8_t opcode, uint32_t nsid,
                           uint32_t cdw10, uint32_t cdw11,
                           uint32_t cdw12, uint64_t prp1, uint32_t *result)
{
    NvmeSqe *cmd = &g_asq[g_asq_tail];
    memset(cmd, 0, sizeof(*cmd));
    cmd->dw0 = (uint32_t)opcode | ((uint32_t)(++g_cid) << 16);
    cmd->nsid = nsid;
    cmd->prp1 = prp1;
    cmd->cdw10 = cdw10;
    cmd->cdw11 = cdw11;
    cmd->cdw12 = cdw12;

    __sync_synchronize();
    g_asq_tail = (uint16_t)((g_asq_tail + 1u) % NVME_ADMIN_Q_SIZE);
    nvme_doorbell(0, false, g_asq_tail);
    return nvme_poll(g_acq, NVME_ADMIN_Q_SIZE, 0, &g_acq_head, &g_acq_phase,
                     result);
}

static uint32_t nvme_io(uint8_t opcode, uint64_t slba, uint32_t nlb,
                        uint64_t prp1, uint64_t prp2)
{
    NvmeSqe *cmd = &g_iosq[g_iosq_tail];
    memset(cmd, 0, sizeof(*cmd));
    cmd->dw0 = (uint32_t)opcode | ((uint32_t)(++g_cid) << 16);
    cmd->nsid = 1;
    cmd->prp1 = prp1;
    cmd->prp2 = prp2;
    cmd->cdw10 = (uint32_t)(slba & 0xFFFFFFFFu);
    cmd->cdw11 = (uint32_t)(slba >> 32);
    cmd->cdw12 = (nlb - 1u) & 0xFFFFu;

    __sync_synchronize();
    g_iosq_tail = (uint16_t)((g_iosq_tail + 1u) % NVME_IO_Q_SIZE);
    nvme_doorbell(1, false, g_iosq_tail);
    return nvme_poll(g_iocq, NVME_IO_Q_SIZE, 1, &g_iocq_head, &g_iocq_phase,
                     NULL);
}

static void nvme_prp(uint64_t base, uint32_t bytes, uint64_t *prp1,
                     uint64_t *prp2)
{
    uint32_t pages = (bytes + PAGE_SIZE - 1u) / PAGE_SIZE;
    *prp1 = base;
    *prp2 = 0;
    if (pages <= 1) return;
    /* two pages: PRP2 is the second page itself, not a list (NVMe 1.4
       4.3) -- a list there had every 8 KB read land in the list page */
    if (pages == 2) {
        *prp2 = base + PAGE_SIZE;
        return;
    }
    for (uint32_t i = 1; i < pages; i++)
        g_prp_list[i - 1] = base + (uint64_t)i * PAGE_SIZE;
    __sync_synchronize();
    *prp2 = g_prp_list_phys;
}

static uint32_t nvme_read_hw(uint32_t lba, uint32_t count, uint64_t prp1,
                             uint64_t prp2)
{
    return nvme_io(NVME_CMD_READ, lba, count, prp1, prp2);
}

static uint32_t nvme_write_hw(uint32_t lba, uint32_t count, uint64_t prp1,
                              uint64_t prp2)
{
    return nvme_io(NVME_CMD_WRITE, lba, count, prp1, prp2);
}

bool nvme_ok(void)
{
    return g_ok;
}

bool nvme_read(uint32_t lba, uint32_t count, void *buf)
{
    if (!g_ok || count == 0) return g_ok;
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    bool dropped = disk_wait_begin();
    bool ok = true;
    uint8_t *out = (uint8_t *)buf;

    for (uint32_t done = 0; ok && done < count; ) {
        uint32_t n = count - done < g_max_sectors ? count - done : g_max_sectors;
        uint32_t bytes = n * 512u;
        uint64_t prp1, prp2;
        nvme_prp(g_bounce_phys, bytes, &prp1, &prp2);
        uint32_t st = nvme_read_hw(lba + done, n, prp1, prp2);
        if (st) {
            nvme_log_err("read", st);
            ok = false;
        } else {
            memcpy(out + (uint64_t)done * 512u, g_bounce, bytes);
        }
        done += n;
    }

    disk_wait_end(dropped);
    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
    return ok;
}

bool nvme_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (!g_ok || count == 0) return g_ok;
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    bool dropped = disk_wait_begin();
    bool ok = true;
    const uint8_t *in = (const uint8_t *)buf;

    for (uint32_t done = 0; ok && done < count; ) {
        uint32_t n = count - done < g_max_sectors ? count - done : g_max_sectors;
        uint32_t bytes = n * 512u;
        uint64_t prp1, prp2;
        memcpy(g_bounce, in + (uint64_t)done * 512u, bytes);
        nvme_prp(g_bounce_phys, bytes, &prp1, &prp2);
        uint32_t st = nvme_write_hw(lba + done, n, prp1, prp2);
        if (st) {
            nvme_log_err("write", st);
            ok = false;
        }
        done += n;
    }

    disk_wait_end(dropped);
    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
    return ok;
}

static bool nvme_alloc_queues(void)
{
    void *p;

    p = pmm_alloc_contiguous(1);
    if (!p) return false;
    g_asq = (NvmeSqe *)P2V((uint64_t)(uintptr_t)p);
    g_asq_phys = (uint64_t)(uintptr_t)p;
    memset(g_asq, 0, PAGE_SIZE);

    p = pmm_alloc_contiguous(1);
    if (!p) return false;
    g_acq = (NvmeCqe *)P2V((uint64_t)(uintptr_t)p);
    g_acq_phys = (uint64_t)(uintptr_t)p;
    memset(g_acq, 0, PAGE_SIZE);

    p = pmm_alloc_contiguous(1);
    if (!p) return false;
    g_iosq = (NvmeSqe *)P2V((uint64_t)(uintptr_t)p);
    g_iosq_phys = (uint64_t)(uintptr_t)p;
    memset(g_iosq, 0, PAGE_SIZE);

    p = pmm_alloc_contiguous(1);
    if (!p) return false;
    g_iocq = (NvmeCqe *)P2V((uint64_t)(uintptr_t)p);
    g_iocq_phys = (uint64_t)(uintptr_t)p;
    memset(g_iocq, 0, PAGE_SIZE);

    p = pmm_alloc_contiguous(1);
    if (!p) return false;
    g_prp_list = (uint64_t *)P2V((uint64_t)(uintptr_t)p);
    g_prp_list_phys = (uint64_t)(uintptr_t)p;
    memset(g_prp_list, 0, PAGE_SIZE);

    p = pmm_alloc_contiguous(1);
    if (!p) return false;
    g_id = (uint8_t *)P2V((uint64_t)(uintptr_t)p);
    g_id_phys = (uint64_t)(uintptr_t)p;
    memset(g_id, 0, PAGE_SIZE);

    p = pmm_alloc_contiguous(NVME_BOUNCE_PAGES);
    if (!p) return false;
    g_bounce = (uint8_t *)P2V((uint64_t)(uintptr_t)p);
    g_bounce_phys = (uint64_t)(uintptr_t)p;

    return true;
}

bool nvme_init(void)
{
    PciDevice dev;
    if (!pci_find_device(0x01, 0x08, &dev)) {
        serial_write_string("NVMe: no NVMe controller on the PCI bus.\r\n");
        return false;
    }
    if (dev.prog_if != 0x02) {
        serial_write_string("NVMe: 01/08 device is not an NVMe controller.\r\n");
        return false;
    }

    uint32_t bar0 = pci_read_config(dev.bus, dev.slot, dev.func, 0x10);
    uint64_t mmio = bar0 & ~0xFULL;
    if ((bar0 & 0x6u) == 0x4u) {
        uint32_t bar1 = pci_read_config(dev.bus, dev.slot, dev.func, 0x14);
        mmio |= (uint64_t)bar1 << 32;
    }
    if (!mmio) {
        serial_write_string("NVMe: BAR0 is not a memory BAR.\r\n");
        return false;
    }

    uint32_t cmd = pci_read_config(dev.bus, dev.slot, dev.func, 0x04);
    cmd |= 0x6u; /* memory space + bus master */
    pci_write_config(dev.bus, dev.slot, dev.func, 0x04, cmd);

    g_regs = (volatile NvmeRegs *)vmm_map_mmio(mmio, 0x4000);
    if (!g_regs) {
        serial_write_string("NVMe: failed to map BAR0.\r\n");
        return false;
    }

    {
        char b[20];
        serial_write_string("NVMe: controller at ");
        uint_to_hex(mmio, b);
        serial_write_string(b);
        serial_write_string(", CAP=");
        uint_to_hex(g_regs->cap, b);
        serial_write_string(b);
        serial_write_string("\r\n");
    }

    g_db_stride = 4u << ((uint32_t)(g_regs->cap >> 32) & 0xFu);

    /* Reset: disable, then re-enable with our queue configuration. */
    if (g_regs->cc & NVME_CC_EN) {
        g_regs->cc &= ~NVME_CC_EN;
        if (!nvme_wait_csts(NVME_CSTS_RDY, 0)) {
            serial_write_string("NVMe: controller did not disable.\r\n");
            return false;
        }
    }

    if (!nvme_alloc_queues()) {
        serial_write_string("NVMe: out of contiguous memory for queues.\r\n");
        return false;
    }
    serial_write_string("NVMe: queues allocated, enabling controller.\r\n");

    g_regs->intms = 0xFFFFFFFFu;
    g_regs->aqa = ((NVME_ADMIN_Q_SIZE - 1u) << 16) | (NVME_ADMIN_Q_SIZE - 1u);
    g_regs->asq_lo = (uint32_t)g_asq_phys;
    g_regs->asq_hi = (uint32_t)(g_asq_phys >> 32);
    g_regs->acq_lo = (uint32_t)g_acq_phys;
    g_regs->acq_hi = (uint32_t)(g_acq_phys >> 32);
    __sync_synchronize();

    g_regs->cc = NVME_CC_EN | (NVME_SQES_64B << NVME_SQES_SHIFT) |
                 (NVME_CQES_16B << NVME_CQES_SHIFT);
    if (!nvme_wait_csts(NVME_CSTS_RDY, NVME_CSTS_RDY)) {
        char b[20];
        serial_write_string("NVMe: controller did not become ready, CSTS=");
        uint_to_hex(g_regs->csts, b);
        serial_write_string(b);
        serial_write_string("\r\n");
        return false;
    }

    /* Identify controller: MDTS caps the transfer size. */
    uint32_t st = nvme_admin(NVME_ADMIN_IDENTIFY, 0, 0x01, 0, 0, g_id_phys,
                             NULL);
    if (st) {
        nvme_log_err("identify controller", st);
        return false;
    }
    {
        uint8_t mdts = g_id[77];
        if (mdts) {
            uint64_t max_bytes = (1ULL << mdts) * PAGE_SIZE;
            if (max_bytes < (uint64_t)g_max_sectors * 512u)
                g_max_sectors = (uint32_t)(max_bytes / 512u);
        }
        if (g_max_sectors == 0) g_max_sectors = 1;
    }

    /* Identify namespace 1: NSZE + LBA format. */
    st = nvme_admin(NVME_ADMIN_IDENTIFY, 1, NVME_CNS_NAMESPACE, 0, 0,
                    g_id_phys, NULL);
    if (st) {
        nvme_log_err("identify namespace", st);
        return false;
    }
    {
        uint64_t nsze;
        memcpy(&nsze, g_id, sizeof(nsze));
        uint8_t flbas = g_id[26];               /* Identify Namespace: FLBAS */
        uint8_t nlba = g_id[25];                /* NLBAF: formats - 1 */
        uint32_t fmt = 0;
        if ((flbas & 0x0Fu) <= nlba && (flbas & 0x0Fu) < 16u)
            memcpy(&fmt, g_id + 0x80 + 4u * (flbas & 0x0Fu), sizeof(fmt));
        uint32_t lbads = (fmt >> 16) & 0xFFu;

        char b[20];
        serial_write_string("NVMe: namespace 1, LBAs=");
        uint_to_hex(nsze, b);
        serial_write_string(b);
        serial_write_string(", LBA shift=");
        uint_to_hex(lbads, b);
        serial_write_string(b);
        serial_write_string("\r\n");

        if (nsze == 0) {
            serial_write_string("NVMe: namespace 1 is empty.\r\n");
            return false;
        }
        g_ns_sectors = nsze;
        if (lbads != 9) {
            serial_write_string("NVMe: only 512-byte LBAs are supported.\r\n");
            return false;
        }
    }

    /* One I/O submission and one I/O completion queue, then create them. */
    uint32_t res = 0;
    st = nvme_admin(NVME_ADMIN_SET_FEATURES, 0, NVME_FEAT_NUM_QUEUES, 0, 0, 0,
                    &res);
    if (st) {
        nvme_log_err("set features (queues)", st);
        return false;
    }
    serial_write_string("NVMe: I/O queues granted, NSQR=");
    {
        char b[20];
        uint_to_hex(res & 0xFFFFu, b);
        serial_write_string(b);
        serial_write_string(" NCQR=");
        uint_to_hex((res >> 16) & 0xFFFFu, b);
        serial_write_string(b);
        serial_write_string("\r\n");
    }

    st = nvme_admin(NVME_ADMIN_CREATE_CQ, 0,
                    ((NVME_IO_Q_SIZE - 1u) << 16) | 1u, 1u, 0, g_iocq_phys,
                    NULL);
    if (st) {
        nvme_log_err("create I/O CQ", st);
        return false;
    }
    st = nvme_admin(NVME_ADMIN_CREATE_SQ, 0,
                    ((NVME_IO_Q_SIZE - 1u) << 16) | 1u, (1u << 16) | 1u, 0,
                    g_iosq_phys, NULL);
    if (st) {
        nvme_log_err("create I/O SQ", st);
        return false;
    }

    /* LBA 0: only our own disk (disklabel.h) is used -- read, never
       written, before that is known. (No write self-test: the last LBA of
       a GPT disk is its backup header.) */
    {
        uint64_t prp1, prp2;
        nvme_prp(g_bounce_phys, 512, &prp1, &prp2);
        st = nvme_read_hw(0, 1, prp1, prp2);
        if (st) {
            nvme_log_err("LBA 0 read", st);
            return false;
        }
        if (!wyn_disk_is_ours(g_bounce)) {
            serial_write_string("NVMe: not a WynlandOS disk -- left alone.\r\n");
            return false;
        }
    }

    serial_write_string("NVMe: ready (polling, 1 admin + 1 I/O queue, "
                        "WynlandOS disk).\r\n");
    g_ok = true;
    return true;
}
