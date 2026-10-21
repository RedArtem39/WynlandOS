/*
 * WynlandOS - USB Mass Storage (Bulk-Only Transport + SCSI)
 */

#include "usb_internal.h"
#include <wynland/pmm.h>
#include <wynland/pmm.h>

#define ST_DATA_BYTES    32768u
#define ST_TIMEOUT_MS    10000u
#define ST_TRIES         3

typedef struct __attribute__((packed)) {
    uint32_t sig;
    uint32_t tag;
    uint32_t dlen;
    uint8_t  flags;
    uint8_t  lun;
    uint8_t  cblen;
    uint8_t  cb[16];
} BotCbw;

typedef struct __attribute__((packed)) {
    uint32_t sig;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;
} BotCsw;

static uint8_t *g_cbw;
static uint8_t *g_csw;
static uint8_t *g_data;
static uint64_t g_cbw_phys, g_csw_phys, g_data_phys;
static uint32_t g_tag = 1;
static uint64_t g_sectors;
static uint32_t g_sector_size = 512;
static uint8_t  g_lun;
static bool g_ready;
static volatile bool g_busy;

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void st_delay_ms(uint32_t ms)
{
    uint64_t start = __builtin_ia32_rdtsc();
    uint64_t limit = (uint64_t)ms * 5000000ULL;
    while (__builtin_ia32_rdtsc() - start < limit)
        __asm__ volatile("pause");
}

static bool bot_once(UsbDev *d, const uint8_t *cb, uint8_t cblen,
                     uint64_t data_phys, uint32_t data_len, bool in,
                     uint32_t *residue)
{
    BotCbw *cbw = (BotCbw *)g_cbw;
    BotCsw *csw = (BotCsw *)g_csw;
    uint32_t tag = g_tag++;

    memset(cbw, 0, sizeof(*cbw));
    cbw->sig = 0x43425355u;                    /* USBC */
    cbw->tag = tag;
    cbw->dlen = data_len;
    cbw->flags = in ? 0x80 : 0x00;
    cbw->lun = g_lun;
    cbw->cblen = cblen;
    memcpy(cbw->cb, cb, cblen);
    __sync_synchronize();

    if (!usb_bulk_xfer(d, d->st_out, &d->st_ring_out, g_cbw_phys,
                       sizeof(*cbw), false, ST_TIMEOUT_MS))
        return false;

    if (data_len) {
        uint8_t ep = in ? d->st_in : d->st_out;
        UsbRing *ring = in ? &d->st_ring_in : &d->st_ring_out;
        if (!usb_bulk_xfer(d, ep, ring, data_phys, data_len, in, ST_TIMEOUT_MS))
            return false;
    }

    memset(csw, 0, sizeof(*csw));
    if (!usb_bulk_xfer(d, d->st_in, &d->st_ring_in, g_csw_phys,
                       sizeof(*csw), true, ST_TIMEOUT_MS))
        return false;

    __sync_synchronize();
    if (csw->sig != 0x53425355u || csw->tag != tag || csw->status != 0)
        return false;
    if (residue) *residue = csw->residue;
    return true;
}

static bool bot_command(UsbDev *d, const uint8_t *cb, uint8_t cblen,
                        uint64_t data_phys, uint32_t data_len, bool in,
                        uint32_t *residue)
{
    for (int i = 0; i < ST_TRIES; i++) {
        if (bot_once(d, cb, cblen, data_phys, data_len, in, residue))
            return true;
        st_delay_ms(50);
    }
    return false;
}

static bool scsi_read_capacity(UsbDev *d, uint32_t *last_lba, uint32_t *bsize)
{
    uint8_t cb[10];
    memset(cb, 0, sizeof(cb));
    cb[0] = 0x25;                              /* READ CAPACITY (10) */
    if (!bot_command(d, cb, 10, g_data_phys, 8, true, NULL))
        return false;
    *last_lba = be32(g_data + 0);
    *bsize = be32(g_data + 4);
    return true;
}

static bool scsi_rw(UsbDev *d, bool in, uint32_t lba, uint32_t count)
{
    uint8_t cb[10];
    memset(cb, 0, sizeof(cb));
    cb[0] = in ? 0x28 : 0x2A;                  /* READ (10) / WRITE (10) */
    put_be32(cb + 2, lba);
    cb[7] = (uint8_t)(count >> 8);
    cb[8] = (uint8_t)count;
    return bot_command(d, cb, 10, g_data_phys, count * g_sector_size, in, NULL);
}

bool usb_storage_ready(void)
{
    return g_ready;
}

uint64_t usb_storage_sectors(void)
{
    return g_sectors;
}

uint32_t usb_storage_sector_size(void)
{
    return g_sector_size;
}

static bool storage_lock(uint32_t timeout_ms)
{
    uint64_t start = __builtin_ia32_rdtsc();
    uint64_t limit = (uint64_t)timeout_ms * 5000000ULL;
    for (;;) {
        uint64_t fl;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
        if (!g_busy) {
            g_busy = true;
            if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
            return true;
        }
        if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
        usb_drain();
        if (__builtin_ia32_rdtsc() - start > limit) return false;
        __asm__ volatile("pause");
    }
}

static void storage_unlock(void)
{
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    g_busy = false;
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

int usb_storage_read_bytes(uint64_t off, void *buf, uint32_t len)
{
    if (!g_ready || !g_usb_storage || !len) return -1;
    if (!storage_lock(15000)) return -1;

    UsbDev *d = g_usb_storage;
    uint8_t *out = (uint8_t *)buf;
    uint32_t ss = g_sector_size;
    uint32_t chunk = ST_DATA_BYTES / ss;

    while (len) {
        uint64_t lba = off / ss;
        uint32_t in_sector = (uint32_t)(off % ss);
        uint32_t nsec = (uint32_t)((in_sector + len + ss - 1) / ss);
        if (nsec > chunk) nsec = chunk;
        if (lba + nsec > g_sectors) { storage_unlock(); return -1; }

        if (!scsi_rw(d, true, (uint32_t)lba, nsec)) {
            storage_unlock();
            return -1;
        }

        uint32_t bytes = nsec * ss - in_sector;
        if (bytes > len) bytes = len;
        memcpy(out, g_data + in_sector, bytes);
        out += bytes;
        off += bytes;
        len -= bytes;
    }

    storage_unlock();
    return 0;
}

int usb_storage_write_bytes(uint64_t off, const void *buf, uint32_t len)
{
    if (!g_ready || !g_usb_storage || !len) return -1;
    if (!storage_lock(15000)) return -1;

    UsbDev *d = g_usb_storage;
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t ss = g_sector_size;
    uint32_t chunk = ST_DATA_BYTES / ss;

    while (len) {
        uint64_t lba = off / ss;
        uint32_t in_sector = (uint32_t)(off % ss);
        uint32_t nsec = (uint32_t)((in_sector + len + ss - 1) / ss);
        if (nsec > chunk) nsec = chunk;
        if (lba + nsec > g_sectors) { storage_unlock(); return -1; }

        /* Partial sectors need their old contents read back first. */
        if (in_sector || len < nsec * ss) {
            if (!scsi_rw(d, true, (uint32_t)lba, nsec)) {
                storage_unlock();
                return -1;
            }
        }
        uint32_t bytes = nsec * ss - in_sector;
        if (bytes > len) bytes = len;
        memcpy(g_data + in_sector, in, bytes);

        if (!scsi_rw(d, false, (uint32_t)lba, nsec)) {
            storage_unlock();
            return -1;
        }
        in += bytes;
        off += bytes;
        len -= bytes;
    }

    storage_unlock();
    return 0;
}

bool usb_storage_setup(UsbDev *d, uint8_t ep_in_addr, uint8_t ep_out_addr,
                       uint16_t maxp_in, uint16_t maxp_out)
{
    uint8_t in_id = (uint8_t)(((ep_in_addr & 0x0F) << 1) | 1u);
    uint8_t out_id = (uint8_t)((ep_out_addr & 0x0F) << 1);
    if (!maxp_in) maxp_in = 64;
    if (!maxp_out) maxp_out = 64;

    void *rin = usb_pages(1, &d->st_ring_in.phys);
    void *rout = usb_pages(1, &d->st_ring_out.phys);
    g_cbw = usb_pages(1, &g_cbw_phys);
    g_csw = usb_pages(1, &g_csw_phys);
    g_data = usb_pages(ST_DATA_BYTES / PAGE_SIZE, &g_data_phys);

    if (!rin || !rout || !g_cbw || !g_csw || !g_data) return false;

    d->st_ring_in.trb = (UsbTrb *)rin;
    d->st_ring_out.trb = (UsbTrb *)rout;
    usb_ring_init(&d->st_ring_in, d->st_ring_in.phys);
    usb_ring_init(&d->st_ring_out, d->st_ring_out.phys);

    d->st_in = in_id;
    d->st_out = out_id;
    d->st_maxp_in = maxp_in;
    d->st_maxp_out = maxp_out;
    d->storage = true;

    if (!usb_configure_ep(d, in_id, 6, maxp_in, 0, &d->st_ring_in))
        return false;
    if (!usb_configure_ep(d, out_id, 2, maxp_out, 0, &d->st_ring_out))
        return false;

    /* GET_MAX_LUN is optional; a STALL means a single LUN, which is fine. */
    uint8_t lun = 0;
    if (usb_control(d, 0xA1, 0xFE, 0, 0, &lun, 1, true))
        g_lun = lun & 0x0F;
    else
        g_lun = 0;

    uint8_t cb[10];
    memset(cb, 0, sizeof(cb));
    cb[0] = 0x12;                              /* INQUIRY */
    cb[4] = 36;
    if (!bot_command(d, cb, 6, g_data_phys, 36, true, NULL)) {
        usb_log("INQUIRY failed");
        return false;
    }

    memset(cb, 0, sizeof(cb));
    bool ready = false;
    for (int i = 0; i < 10 && !ready; i++) {
        ready = bot_once(d, cb, 6, 0, 0, false, NULL);
        if (!ready) st_delay_ms(100);
    }
    if (!ready) {
        usb_log("TEST UNIT READY failed");
        return false;
    }

    uint32_t last = 0, bs = 0;
    if (!scsi_read_capacity(d, &last, &bs)) {
        usb_log("READ CAPACITY failed");
        return false;
    }
    if (bs != 512 && bs != 1024 && bs != 2048 && bs != 4096) {
        usb_log_hex("unsupported block size=", bs);
        return false;
    }

    g_sectors = (uint64_t)last + 1u;
    g_sector_size = bs;
    g_usb_storage = d;

    /* Read self-test: LBA 0 must be readable (and, for an ext2 stick, carry
       the superblock magic at 1024+56). */
    if (!scsi_rw(d, true, 0, 3)) {
        usb_log("storage read self-test failed");
        return false;
    }
    __sync_synchronize();
    usb_log_hex("storage LBA0 magic=",
                (uint64_t)g_data[1080] | ((uint64_t)g_data[1081] << 8));

    g_ready = true;
    usb_log_hex("mass storage sectors=", g_sectors);
    return true;
}
