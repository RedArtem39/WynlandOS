/*
 * WynlandOS - Intel Wi-Fi (AX201 and the rest of the 22000 family, Qu /
 * QuZ), ported from OpenBSD's iwx(4) (sys/dev/pci/if_iwx.c):
 *
 *   Copyright (c) 2014, 2016 genua gmbh <info@genua.de>
 *     Author: Stefan Sperling <stsp@openbsd.org>
 *   Copyright (c) 2014 Fixup Software Ltd.
 *   Copyright (c) 2017, 2019, 2020 Stefan Sperling <stsp@openbsd.org>
 *   (and Intel's, through it: the iwlwifi driver it follows)
 *
 *   Permission to use, copy, modify, and distribute this software for any
 *   purpose with or without fee is hereby granted, provided that the above
 *   copyright notice and this permission notice appear in all copies.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *   WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *   MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *   ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *   ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *   OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Step 1 of the Wi-Fi plan: the card found, woken, its firmware loaded
 * and running ("alive"). The firmware (Intel's iwlwifi image, API 77, in
 * /lib/firmware: tools/stage_wifi.sh) is chosen from the card's revision.
 * The card loads itself from host memory: a "context info" block tells it
 * where the firmware's sections are and where the RX queue and the
 * command queue live; the driver kicks it and waits for the ALIVE
 * interrupt, then for the ALIVE notification on the RX queue.
 *
 * No interrupts yet: the cause register is polled (INTx kept off, MSI not
 * enabled -- nothing reaches the CPU). Every step goes to the stick's log
 * at once, so a hang shows where. A thread of its own, a few seconds
 * after boot; boot.cfg "wifi=0" keeps it off.
 */

#include <wynland/types.h>
#include <wynland/pci.h>
#include <wynland/pmm.h>
#include <wynland/vmm.h>
#include <wynland/memlayout.h>
#include <wynland/sched.h>
#include <wynland/vfs.h>

extern void serial_write_string(const char *s);
extern void uint_to_hex(uint64_t v, char *buf);
extern uint64_t timer_get_ms(void);
extern void *memset(void *d, int c, size_t n);
extern void *memcpy(void *d, const void *s, size_t n);
extern void sticklog_panic_flush(void);

/* ---- registers (if_iwxreg.h) ---------------------------------------- */
#define CSR_HW_IF_CONFIG_REG        0x000
#define CSR_INT_COALESCING          0x004
#define CSR_INT                     0x008
#define CSR_INT_MASK                0x00c
#define CSR_FH_INT_STATUS           0x010
#define CSR_RESET                   0x020
#define CSR_GP_CNTRL                0x024
#define CSR_HW_REV                  0x028
#define CSR_GIO_REG                 0x03C
#define CSR_CTXT_INFO_BA            0x040
#define CSR_UCODE_DRV_GP1_CLR       0x05c
#define CSR_MBOX_SET_REG            0x088
#define CSR_HW_RF_ID                0x09c
#define CSR_MAC_SHADOW_REG_CTRL     0x0A8
#define CSR_LTR_LONG_VAL_AD         0x0d4
#define CSR_GIO_CHICKEN_BITS        0x100
#define CSR_DBG_HPET_MEM_REG        0x240
#define CSR_DBG_LINK_PWR_MGMT_REG   0x250
#define HBUS_TARG_PRPH_WADDR        0x444
#define HBUS_TARG_PRPH_RADDR        0x448
#define HBUS_TARG_PRPH_WDAT         0x44c
#define HBUS_TARG_PRPH_RDAT         0x450
#define RFH_Q0_FRBDCB_WIDX_TRG      0x1C80

#define GP_MAC_CLOCK_READY          0x00000001
#define GP_INIT_DONE                0x00000004
#define GP_MAC_ACCESS_REQ           0x00000008
#define GP_GOING_TO_SLEEP           0x00000010
#define GP_RFKILL_WAKE_L1A_EN       0x04000000
#define GP_HW_RF_KILL_SW            0x08000000
#define RESET_SW_RESET              0x00000080
#define HWIF_NIC_READY              0x00400000
#define HWIF_PREPARE                0x08000000
#define HWIF_HAP_WAKE_L1A           0x00080000
#define HWIF_MSK                    (0x3 | 0xC | 0x100 | 0x200 | 0xC00 | 0x3000 | 0xC000)
#define LINK_PWR_MGMT_DISABLED      0x80000000
#define HPET_MEM_VAL                0xFFFF0000
#define MBOX_OS_ALIVE               0x20
#define GIO_L0S_DISABLED            0x00000002
#define CHICKEN_L1A_NO_L0S_RX       0x00800000
#define UCODE_SW_BIT_RFKILL         0x00000002
#define UCODE_GP1_CMD_BLOCKED       0x00000004
#define INT_ALIVE                   (1u << 0)
#define INT_SW_RX                   (1u << 3)
#define INT_RF_KILL                 (1u << 7)
#define INT_SW_ERR                  (1u << 25)
#define INT_HW_ERR                  (1u << 29)
#define INT_FH_RX                   (1u << 31)

/* periphery registers, through HBUS_TARG_PRPH_* */
#define UREG_CPU_INIT_RUN           0xa05c44
#define UREG_CHICK                  0xa05c00
#define UREG_CHICK_MSI_ENABLE       (1u << 24)
#define HPM_DEBUG                   0xa03440
#define PERSISTENCE_BIT             (1u << 12)
#define PREG_PRPH_WPROT_22000       0xa04d00
#define PREG_WFPM_ACCESS            (1u << 12)
#define HPM_HIPM_GEN_CFG            0xa03458
#define HIPM_CR_PG_EN               (1u << 0)
#define HIPM_CR_SLP_EN              (1u << 1)
#define HIPM_CR_FORCE_ACTIVE        (1u << 10)
#define HPM_MAC_LTR_CSR             0xa0348c
#define HPM_MAC_LTR_ENABLE_ALL      0xf
#define HPM_UMAC_LTR                0xa03480

#define HW_REV_TYPE_QUZ             0x354
#define HW_REV_TYPE_QU_C0           0x338
#define RF_TYPE_JF1                 0x108
#define RF_TYPE_JF2                 0x105

/* the firmware file */
#define TLV_UCODE_MAGIC             0x0a4c5749
#define TLV_SEC_RT                  19
#define TLV_PHY_SKU                 23
#define TLV_FW_VERSION              36
#define SEP_CPU1_CPU2               0xFFFFCCCCu
#define SEP_PAGING                  0xAAAABBBBu
#define MAX_SECT                    69
#define MAX_DRAM_ENTRY              64

/* queues */
#define RX_RING                     512         /* IWX_RX_MQ_RING_COUNT = IWX_MQ_RX_TABLE_SIZE */
#define RX_CB_SIZE                  9           /* log2(512) */
#define RBUF_SIZE                   4096
#define TX_RING                     256
#define TX_CB_SIZE                  5           /* log2(256) - 3 */
#define TFD_SIZE                    256         /* struct iwx_tfh_tfd */
#define CTXT_TFD_FORMAT_LONG        (1u << 8)
#define CTXT_RB_CB_SIZE_POS         4
#define CTXT_RB_SIZE_POS            9
#define CTXT_RB_SIZE_4K             0x4
#define FH_FRAME_SIZE_MSK           0x00003fff
#define FH_FRAME_INVALID            0x55550000u
#define FH_FRAME_ALIGN              0x40
#define CMD_ALIVE                   0x1
#define ALIVE_STATUS_OK             0xCAFE

/* the context info the card reads to load itself (struct iwx_context_info) */
typedef struct __attribute__((packed)) {
    uint16_t mac_id, version, size, reserved;
    uint32_t control_flags, control_reserved;
    uint64_t reserved0;
    uint64_t free_rbd_addr, used_rbd_addr, status_wr_ptr;
    uint64_t cmd_queue_addr;
    uint8_t  cmd_queue_size, hcmd_reserved[7];
    uint32_t reserved1[4];
    uint64_t core_dump_addr; uint32_t core_dump_size, dump_reserved;
    uint64_t early_debug_addr; uint32_t early_debug_size, edbg_reserved;
    uint64_t platform_nvm_addr; uint32_t platform_nvm_size, pnvm_reserved;
    uint32_t reserved2[16];
    uint64_t umac_img[MAX_DRAM_ENTRY];
    uint64_t lmac_img[MAX_DRAM_ENTRY];
    uint64_t virtual_img[MAX_DRAM_ENTRY];
    uint32_t reserved3[16];
} CtxtInfo;
_Static_assert(sizeof(CtxtInfo) == 1792, "struct iwx_context_info");

typedef struct { uint64_t phys; void *virt; uint32_t pages; } Dma;
typedef struct { uint32_t devoff; const uint8_t *data; uint32_t len; } Sect;

static volatile uint8_t *g_mmio;
static uint8_t g_bus, g_slot, g_func;
static uint32_t g_hw_rev, g_hw_rf_id, g_phy_config;
static int g_nic_locks;
static Sect g_sect[MAX_SECT];
static int g_nsect;
static char g_fwver[48];
static Dma g_ctxt, g_rx_free, g_rx_used, g_rx_stat, g_txq;
static Dma g_rxbuf[RX_RING];
static int g_rx_cur;

/* ---- the log -------------------------------------------------------- */
static void say(const char *s, uint64_t v)
{
    char b[20];
    serial_write_string("wifi: ");
    serial_write_string(s);
    uint_to_hex(v, b);
    serial_write_string(b);
    serial_write_string("\r\n");
}
static void sayx(const char *s)
{
    serial_write_string("wifi: ");
    serial_write_string(s);
    serial_write_string("\r\n");
}
/* a step: said and on the stick at once (a hang leaves its last step) */
static void step(const char *s)
{
    sayx(s);
    sticklog_panic_flush();
}

/* ---- access ---------------------------------------------------------- */
static inline uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(g_mmio + r); }
static inline void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(g_mmio + r) = v; }
static inline void wr8(uint32_t r, uint8_t v) { *(volatile uint8_t *)(g_mmio + r) = v; }
static inline void setb(uint32_t r, uint32_t b) { wr(r, rd(r) | b); }
static inline void clrb(uint32_t r, uint32_t b) { wr(r, rd(r) & ~b); }

static void udelay(uint32_t us)
{
    for (volatile uint32_t i = 0; i < us * 40; i++) __asm__ volatile("pause");
}
static void mdelay(uint32_t ms)
{
    if (ms >= 10) { sched_sleep_ms(ms); return; }
    udelay(ms * 1000);
}

/* bits == mask of the register within timo microseconds */
static bool poll_bit(uint32_t r, uint32_t bits, uint32_t mask, uint32_t timo)
{
    for (;;) {
        if ((rd(r) & mask) == (bits & mask)) return true;
        if (timo < 10) return false;
        timo -= 10;
        udelay(10);
    }
}

static uint32_t rd_prph_unlocked(uint32_t a)
{
    wr(HBUS_TARG_PRPH_RADDR, (a & 0x000fffff) | (3u << 24));
    __asm__ volatile("mfence" ::: "memory");
    return rd(HBUS_TARG_PRPH_RDAT);
}
static void wr_prph_unlocked(uint32_t a, uint32_t v)
{
    wr(HBUS_TARG_PRPH_WADDR, (a & 0x000fffff) | (3u << 24));
    __asm__ volatile("sfence" ::: "memory");
    wr(HBUS_TARG_PRPH_WDAT, v);
}

static bool nic_lock(void)
{
    if (g_nic_locks > 0) { g_nic_locks++; return true; }
    setb(CSR_GP_CNTRL, GP_MAC_ACCESS_REQ);
    udelay(2);
    if (poll_bit(CSR_GP_CNTRL, GP_MAC_CLOCK_READY, GP_MAC_CLOCK_READY | GP_GOING_TO_SLEEP, 150000)) {
        g_nic_locks++;
        return true;
    }
    say("acquiring the device failed, GP_CNTRL ", rd(CSR_GP_CNTRL));
    return false;
}
static void nic_unlock(void)
{
    if (g_nic_locks > 0 && --g_nic_locks == 0) clrb(CSR_GP_CNTRL, GP_MAC_ACCESS_REQ);
}
static bool set_bits_mask_prph(uint32_t reg, uint32_t bits, uint32_t mask)
{
    if (!nic_lock()) return false;
    wr_prph_unlocked(reg, (rd_prph_unlocked(reg) & mask) | bits);
    nic_unlock();
    return true;
}

/* ---- DMA memory: physically contiguous, zeroed (x86 DMA is coherent) */
static bool dma_alloc(Dma *d, uint32_t bytes)
{
    uint32_t pages = (bytes + 4095) / 4096;
    void *p = pmm_alloc_contiguous(pages);
    if (!p) return false;
    d->phys = (uint64_t)(uintptr_t)p;
    d->virt = P2V(d->phys);
    d->pages = pages;
    memset(d->virt, 0, (size_t)pages * 4096);
    return true;
}

/* ---- the card up (iwx_prepare_card_hw, iwx_start_hw, iwx_apm_init) --- */
static bool set_hw_ready(void)
{
    setb(CSR_HW_IF_CONFIG_REG, HWIF_NIC_READY);
    bool ready = poll_bit(CSR_HW_IF_CONFIG_REG, HWIF_NIC_READY, HWIF_NIC_READY, 50);
    if (ready) setb(CSR_MBOX_SET_REG, MBOX_OS_ALIVE);
    return ready;
}

static bool prepare_card_hw(void)
{
    if (set_hw_ready()) return true;
    setb(CSR_DBG_LINK_PWR_MGMT_REG, LINK_PWR_MGMT_DISABLED);
    mdelay(1);
    for (int n = 0; n < 10; n++) {
        setb(CSR_HW_IF_CONFIG_REG, HWIF_PREPARE);
        for (uint32_t t = 0; t < 150000; t += 200) {
            if (set_hw_ready()) return true;
            udelay(200);
        }
        mdelay(25);
    }
    return false;
}

static bool clear_persistence_bit(void)
{
    uint32_t hpm = rd_prph_unlocked(HPM_DEBUG);
    if (hpm != 0xa5a5a5a0 && (hpm & PERSISTENCE_BIT)) {
        if (rd_prph_unlocked(PREG_PRPH_WPROT_22000) & PREG_WFPM_ACCESS) {
            sayx("cannot clear the persistence bit");
            return false;
        }
        wr_prph_unlocked(HPM_DEBUG, hpm & ~PERSISTENCE_BIT);
    }
    return true;
}

static void sw_reset(void)
{
    setb(CSR_RESET, RESET_SW_RESET);
    mdelay(5);
}

static bool force_power_gating(void)
{
    if (!set_bits_mask_prph(HPM_HIPM_GEN_CFG, HIPM_CR_FORCE_ACTIVE, ~0u)) return false;
    udelay(20);
    if (!set_bits_mask_prph(HPM_HIPM_GEN_CFG, HIPM_CR_PG_EN | HIPM_CR_SLP_EN, ~0u)) return false;
    udelay(20);
    return set_bits_mask_prph(HPM_HIPM_GEN_CFG, 0, ~HIPM_CR_FORCE_ACTIVE);
}

static bool apm_init(void)
{
    setb(CSR_GIO_CHICKEN_BITS, CHICKEN_L1A_NO_L0S_RX);
    setb(CSR_DBG_HPET_MEM_REG, HPET_MEM_VAL);
    setb(CSR_HW_IF_CONFIG_REG, HWIF_HAP_WAKE_L1A);
    setb(CSR_GIO_REG, GIO_L0S_DISABLED);
    setb(CSR_GP_CNTRL, GP_INIT_DONE);
    if (!poll_bit(CSR_GP_CNTRL, GP_MAC_CLOCK_READY, GP_MAC_CLOCK_READY, 25000)) {
        say("timeout waiting for the clock, GP_CNTRL ", rd(CSR_GP_CNTRL));
        return false;
    }
    return true;
}

static bool start_hw(void)
{
    step("prepare the card");
    if (!prepare_card_hw()) { say("the card did not get ready, HW_IF_CONFIG ", rd(CSR_HW_IF_CONFIG_REG)); return false; }
    step("clear the persistence bit");
    if (!clear_persistence_bit()) return false;
    step("reset");
    sw_reset();
    /* (22000, integrated: up once, power gating forced, reset again) */
    setb(CSR_GP_CNTRL, GP_INIT_DONE);
    udelay(20);
    if (!poll_bit(CSR_GP_CNTRL, GP_MAC_CLOCK_READY, GP_MAC_CLOCK_READY, 25000)) {
        say("timeout waiting for the clock (power gating), GP_CNTRL ", rd(CSR_GP_CNTRL));
        return false;
    }
    step("force power gating");
    if (!force_power_gating()) return false;
    sw_reset();
    step("APM init");
    if (!apm_init()) return false;
    /* interrupts by MSI, not MSI-X (polled here anyway: neither reaches
       the CPU) */
    if (nic_lock()) { wr_prph_unlocked(UREG_CHICK, UREG_CHICK_MSI_ENABLE); nic_unlock(); }
    wr(CSR_INT_MASK, INT_RF_KILL);
    setb(CSR_GP_CNTRL, GP_RFKILL_WAKE_L1A_EN);
    if (!(rd(CSR_GP_CNTRL) & GP_HW_RF_KILL_SW)) step("RADIO OFF: the hardware RF-kill switch (airplane mode key?)");
    return true;
}

/* ---- the firmware file (iwx_read_firmware) --------------------------- */
static uint8_t *g_fw;
static uint32_t g_fw_len;

static bool read_firmware(const char *path)
{
    VfsFile *f = vfs_open(path);
    if (!f) { sayx("no firmware file:"); sayx(path); return false; }
    g_fw_len = f->node.size;
    void *p = pmm_alloc_contiguous((g_fw_len + 4095) / 4096);
    if (!p) { vfs_close(f); sayx("no memory for the firmware"); return false; }
    g_fw = (uint8_t *)P2V((uint64_t)(uintptr_t)p);
    uint32_t got = 0;
    while (got < g_fw_len) {
        int n = vfs_read(f, g_fw + got, g_fw_len - got > 65536 ? 65536 : g_fw_len - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    vfs_close(f);
    if (got != g_fw_len) { say("firmware short read, got ", got); return false; }

    const uint32_t *h = (const uint32_t *)g_fw;
    if (g_fw_len < 88 || h[0] != 0 || h[1] != TLV_UCODE_MAGIC) { sayx("not an iwlwifi TLV firmware"); return false; }
    uint32_t ver = *(const uint32_t *)(g_fw + 72);
    say("firmware version (major) ", ver >> 24);
    say("firmware version (minor) ", (ver >> 16) & 0xFF);
    say("firmware API ", (ver >> 8) & 0xFF);
    const uint8_t *d = g_fw + 88;
    uint32_t len = g_fw_len - 88;
    g_nsect = 0;
    while (len >= 8) {
        uint32_t type = ((const uint32_t *)d)[0], tl = ((const uint32_t *)d)[1];
        d += 8; len -= 8;
        if (tl > len) { say("firmware too short at TLV type ", type); return false; }
        if (type == TLV_SEC_RT) {
            if (g_nsect >= MAX_SECT || tl < 4) { sayx("too many firmware sections"); return false; }
            g_sect[g_nsect].devoff = *(const uint32_t *)d;
            g_sect[g_nsect].data = d + 4;
            g_sect[g_nsect].len = tl - 4;
            g_nsect++;
        } else if (type == TLV_PHY_SKU && tl == 4) {
            g_phy_config = *(const uint32_t *)d;
        } else if (type == TLV_FW_VERSION && tl == 12) {
            const uint32_t *v = (const uint32_t *)d;
            say("firmware version TLV: major ", v[0]);
            say("firmware version TLV: minor ", v[1]);
            say("firmware version TLV: local ", v[2]);
            g_fwver[0] = 1;
        }
        uint32_t adv = (tl + 3) & ~3u;
        if (adv > len) break;
        d += adv; len -= adv;
    }
    say("runtime firmware sections ", g_nsect);
    return g_nsect > 0;
}

/* ---- the queues and the context info (iwx_ctxt_info_init) ------------ */
static bool alloc_queues(void)
{
    if (!dma_alloc(&g_rx_free, RX_RING * 8) || !dma_alloc(&g_rx_used, RX_RING * 4) ||
        !dma_alloc(&g_rx_stat, 16) || !dma_alloc(&g_txq, TX_RING * TFD_SIZE) || !dma_alloc(&g_ctxt, sizeof(CtxtInfo))) {
        sayx("no memory for the queues");
        return false;
    }
    for (int i = 0; i < RX_RING; i++)
        if (!dma_alloc(&g_rxbuf[i], RBUF_SIZE)) { say("no memory for RX buffer ", i); return false; }
    return true;
}

/* the sections up to a separator */
static int sect_count(int start)
{
    int n = 0;
    while (start < g_nsect && g_sect[start].devoff != SEP_CPU1_CPU2 && g_sect[start].devoff != SEP_PAGING) { start++; n++; }
    return n;
}
/* a section copied into DMA memory; its physical address (0: no memory) */
static uint64_t sect_dma(int i)
{
    Dma d;
    if (!dma_alloc(&d, g_sect[i].len)) return 0;
    memcpy(d.virt, g_sect[i].data, g_sect[i].len);
    return d.phys;
}

static bool ctxt_info_init(void)
{
    CtxtInfo *ci = (CtxtInfo *)g_ctxt.virt;
    memset(ci, 0, sizeof *ci);
    ci->mac_id = (uint16_t)rd(CSR_HW_REV);
    ci->version = 0;
    ci->size = sizeof(CtxtInfo) / 4;
    ci->control_flags = CTXT_TFD_FORMAT_LONG | (RX_CB_SIZE << CTXT_RB_CB_SIZE_POS) | (CTXT_RB_SIZE_4K << CTXT_RB_SIZE_POS);
    ci->free_rbd_addr = g_rx_free.phys;
    ci->used_rbd_addr = g_rx_used.phys;
    ci->status_wr_ptr = g_rx_stat.phys;
    ci->cmd_queue_addr = g_txq.phys;
    ci->cmd_queue_size = TX_CB_SIZE;

    int lmac = sect_count(0), umac = sect_count(lmac + 1), paging = sect_count(lmac + umac + 2);
    say("LMAC sections ", lmac);
    say("UMAC sections ", umac);
    say("paging sections ", paging);
    if (lmac > MAX_DRAM_ENTRY || umac > MAX_DRAM_ENTRY || paging > MAX_DRAM_ENTRY) return false;
    for (int i = 0; i < lmac; i++) { uint64_t a = sect_dma(i); if (!a) return false; ci->lmac_img[i] = a; }
    for (int i = 0; i < umac; i++) { uint64_t a = sect_dma(lmac + 1 + i); if (!a) return false; ci->umac_img[i] = a; }
    for (int i = 0; i < paging; i++) { uint64_t a = sect_dma(lmac + umac + 2 + i); if (!a) return false; ci->virtual_img[i] = a; }
    __asm__ volatile("mfence" ::: "memory");

    /* the 64-bit address in two writes (one 64-bit write fails on the AX201) */
    wr(CSR_CTXT_INFO_BA, (uint32_t)g_ctxt.phys);
    wr(CSR_CTXT_INFO_BA + 4, (uint32_t)(g_ctxt.phys >> 32));

    if (!nic_lock()) return false;
    /* LTR ~250 us during the boot (iwx_set_ltr: integrated 22000) */
    /* (computed as iwx_set_ltr does, its masks and all: the snoop scale
       lands outside its mask there too) */
    const uint32_t ltr = 0x80000000u | ((2u << 24) & 0x1c000000u) | ((250u << 16) & 0x03ff0000u) |
                         0x00008000u | ((2u << 8) & 0x00001c00u) | (250u & 0x3ffu);
    wr_prph_unlocked(HPM_MAC_LTR_CSR, HPM_MAC_LTR_ENABLE_ALL);
    wr_prph_unlocked(HPM_UMAC_LTR, ltr);
    /* kick the firmware's self load */
    wr_prph_unlocked(UREG_CPU_INIT_RUN, 1);
    nic_unlock();
    return true;
}

/* iwx_nic_config: the MAC and radio steps from the revision and the PHY SKU */
static void nic_config(void)
{
    uint32_t v = 0;
    v |= ((g_hw_rev & 0xC) >> 2) << 2;
    v |= (g_hw_rev & 0x3) << 0;
    v |= ((g_phy_config >> 0) & 3) << 10;
    v |= ((g_phy_config >> 2) & 3) << 14;
    v |= ((g_phy_config >> 4) & 3) << 12;
    wr(CSR_HW_IF_CONFIG_REG, (rd(CSR_HW_IF_CONFIG_REG) & ~HWIF_MSK) | v);
}

/* ---- the RX queue: what the firmware says ---------------------------- */
static bool g_alive_irq, g_alive_ok;

static void rx_drain(void)
{
    uint16_t hw = *(volatile uint16_t *)g_rx_stat.virt & 0xfff;
    hw &= RX_RING - 1;
    int seen = 0;
    while (g_rx_cur != hw && seen++ < RX_RING) {
        const uint8_t *b = (const uint8_t *)g_rxbuf[g_rx_cur].virt;
        uint32_t off = 0;
        while (off + 8 < RBUF_SIZE) {
            uint32_t lnf = *(const uint32_t *)(b + off);
            uint8_t code = b[off + 4], grp = b[off + 5], idx = b[off + 6], qid = b[off + 7];
            if ((qid == 0 && idx == 0 && code == 0 && grp == 0) || lnf == FH_FRAME_INVALID) break;
            uint32_t len = 4 + (lnf & FH_FRAME_SIZE_MSK);
            serial_write_string("wifi: notification group ");
            char t[20]; uint_to_hex(grp, t); serial_write_string(t);
            serial_write_string(" code "); uint_to_hex(code, t); serial_write_string(t);
            serial_write_string(" length "); uint_to_hex(len, t); serial_write_string(t);
            serial_write_string("\r\n");
            if (grp == 0 && code == CMD_ALIVE && len >= 10) {
                uint16_t status = *(const uint16_t *)(b + off + 8);
                say("ALIVE notification, status ", status);
                if (status == ALIVE_STATUS_OK) g_alive_ok = true;
            }
            if (len < 8 || len > RBUF_SIZE - off) break;
            off += (len + FH_FRAME_ALIGN - 1) & ~(uint32_t)(FH_FRAME_ALIGN - 1);
        }
        g_rx_cur = (g_rx_cur + 1) % RX_RING;
    }
    /* what has been read, back to the card (aligned by 8, as iwx does) */
    uint16_t back = hw == 0 ? RX_RING - 1 : hw - 1;
    wr(RFH_Q0_FRBDCB_WIDX_TRG, back & ~7u);
}

/* the cause register, polled: ALIVE, RX, errors */
static void poll_causes(uint64_t until)
{
    while (timer_get_ms() < until && !g_alive_ok) {
        uint32_t r1 = rd(CSR_INT);
        if (r1 == 0xffffffffu || (r1 & 0xfffffff0u) == 0xa5a5a5a0u) { say("the card is gone, CSR_INT ", r1); return; }
        uint32_t r2 = rd(CSR_FH_INT_STATUS);
        if (r1 || r2) {
            wr(CSR_INT, r1);
            wr(CSR_FH_INT_STATUS, r2);
            if (r1 & INT_ALIVE) {
                step("ALIVE interrupt: the firmware runs");
                g_alive_irq = true;
                /* the firmware has set the RX queue up: the buffers to it */
                uint64_t *desc = (uint64_t *)g_rx_free.virt;
                for (int i = 0; i < RX_RING; i++) desc[i] = g_rxbuf[i].phys | (uint64_t)(i & 0x0fff);
                __asm__ volatile("mfence" ::: "memory");
                wr(RFH_Q0_FRBDCB_WIDX_TRG, 8);
            }
            if (r1 & (INT_SW_ERR | INT_HW_ERR)) { say("FIRMWARE / HARDWARE ERROR, CSR_INT ", r1); return; }
            if (r1 & INT_RF_KILL) say("RF-kill switch changed, GP_CNTRL ", rd(CSR_GP_CNTRL));
            if ((r1 & (INT_FH_RX | INT_SW_RX)) || r2) rx_drain();
        }
        sched_sleep_ms(1);
    }
}

/* ---- the thread ------------------------------------------------------ */
static bool find_card(void)
{
    PciDevice d;
    if (!pci_find_device(0x02, 0x80, &d) || d.vendor_id != 0x8086) return false;
    g_bus = d.bus; g_slot = d.slot; g_func = d.func;
    say("Intel wireless card: device ", d.device_id);
    say("  at bus ", g_bus);
    say("  slot ", g_slot);
    say("  function ", g_func);
    say("  subsystem ", pci_read_config(g_bus, g_slot, g_func, 0x2C));
    say("  revision ", pci_read_config(g_bus, g_slot, g_func, 0x08) & 0xFF);
    return true;
}

static void wifi_thread(void *arg)
{
    (void)arg;
    sched_sleep_ms(4000);                       /* (after the GPU's bring-up) */
    step("looking for an Intel wireless card");
    if (!find_card()) { sayx("no Intel wireless card"); return; }

    /* D0, memory and bus mastering on, INTx off; the retry timeout off
       (iwx: PCI Tx retries interfere with C3) */
    uint8_t cap = pci_read_config(g_bus, g_slot, g_func, 0x34) & 0xFC;
    for (int n = 0; cap && n < 48; n++) {
        uint32_t h = pci_read_config(g_bus, g_slot, g_func, cap);
        if ((h & 0xFF) == 0x01) {               /* power management */
            uint32_t pmcsr = pci_read_config(g_bus, g_slot, g_func, cap + 4);
            if (pmcsr & 3) {
                say("the card was in D", pmcsr & 3);
                pci_write_config(g_bus, g_slot, g_func, cap + 4, pmcsr & ~3u);
                sched_sleep_ms(10);
            }
        }
        cap = (h >> 8) & 0xFC;
    }
    uint32_t r40 = pci_read_config(g_bus, g_slot, g_func, 0x40);
    pci_write_config(g_bus, g_slot, g_func, 0x40, r40 & ~0xff00u);
    uint32_t cmd = pci_read_config(g_bus, g_slot, g_func, 0x04);
    pci_write_config(g_bus, g_slot, g_func, 0x04, (cmd | 0x6u | (1u << 10)) & 0xFFFFu);
    uint32_t lo = pci_read_config(g_bus, g_slot, g_func, 0x10), hi = 0;
    if ((lo & 0x6) == 0x4) hi = pci_read_config(g_bus, g_slot, g_func, 0x14);
    uint64_t bar0 = ((uint64_t)hi << 32) | (lo & ~0xFull);
    say("BAR0 ", bar0);
    if (!bar0) { sayx("BAR0 not assigned"); return; }
    g_mmio = (volatile uint8_t *)vmm_map_mmio(bar0, 0x4000);
    if (!g_mmio) { sayx("BAR0 not mapped"); return; }

    wr(CSR_INT_MASK, 0);
    wr(CSR_INT, ~0u);
    wr(CSR_FH_INT_STATUS, ~0u);
    uint32_t raw_rev = rd(CSR_HW_REV);
    g_hw_rf_id = rd(CSR_HW_RF_ID);
    say("CSR_HW_REV ", raw_rev);
    say("CSR_HW_RF_ID ", g_hw_rf_id);
    if (raw_rev == 0xffffffffu) { sayx("the card does not answer (all ones)"); return; }
    g_hw_rev = (raw_rev & 0xfff0) | ((((raw_rev << 2) & 0xC) >> 2) << 2);

    /* the firmware for this card (iwx_attach, the 22000 family) */
    const char *fw;
    uint32_t type = g_hw_rev & 0xfff0, mstep = (g_hw_rev >> 2) & 3, rft = (g_hw_rf_id & 0x0FFF000) >> 12;
    if (type == (HW_REV_TYPE_QU_C0 & 0xfff0) && mstep >= 2)          /* Qu, C step */
        fw = "/lib/firmware/iwlwifi-Qu-c0-hr-b0-77.ucode";
    else if (type == (HW_REV_TYPE_QUZ & 0xfff0))                       /* QuZ (Tiger Lake's) */
        fw = (rft == RF_TYPE_JF1 || rft == RF_TYPE_JF2) ? "/lib/firmware/iwlwifi-QuZ-a0-jf-b0-77.ucode"
                                                        : "/lib/firmware/iwlwifi-QuZ-a0-hr-b0-77.ucode";
    else
        fw = "/lib/firmware/iwlwifi-Qu-b0-hr-b0-77.ucode";
    say("hardware revision (normalised) ", g_hw_rev);
    say("radio type ", rft);
    step(fw);

    if (!read_firmware(fw)) return;
    if (!start_hw()) return;
    step("allocate the queues");
    if (!alloc_queues()) return;

    /* iwx_start_fw */
    step("start the firmware");
    wr(CSR_INT, ~0u);
    wr(CSR_INT_MASK, 0);
    wr(CSR_UCODE_DRV_GP1_CLR, UCODE_SW_BIT_RFKILL);
    wr(CSR_UCODE_DRV_GP1_CLR, UCODE_GP1_CMD_BLOCKED);
    wr(CSR_INT, ~0u);
    if (!apm_init()) return;
    nic_config();
    wr8(CSR_INT_COALESCING, 0x40);
    setb(CSR_MAC_SHADOW_REG_CTRL, 0x800fffff);
    wr(CSR_INT_MASK, INT_ALIVE | INT_FH_RX);
    if (!ctxt_info_init()) { sayx("the context info failed"); return; }
    step("waiting for the firmware");
    poll_causes(timer_get_ms() + 3000);
    if (g_alive_ok) step("FIRMWARE ALIVE -- step 1 done");
    else if (g_alive_irq) step("the ALIVE interrupt came, its notification not (or not OK)");
    else {
        say("NO ALIVE in 3 s, CSR_INT ", rd(CSR_INT));
        say("  GP_CNTRL ", rd(CSR_GP_CNTRL));
        say("  RESET ", rd(CSR_RESET));
        say("  HW_IF_CONFIG ", rd(CSR_HW_IF_CONFIG_REG));
        sticklog_panic_flush();
    }
}

/* boot.cfg "wifi=0": off */
static bool cfg_has(const char *cfg, const char *line)
{
    for (int i = 0; cfg && cfg[i]; i++) {
        if (i && cfg[i - 1] != '\n') continue;
        int k = 0;
        while (line[k] && cfg[i + k] == line[k]) k++;
        if (!line[k] && (cfg[i + k] == '\n' || cfg[i + k] == '\r' || !cfg[i + k])) return true;
    }
    return false;
}

void iwx_start(const char *cfg)
{
    if (cfg_has(cfg, "wifi=0")) { sayx("off (boot.cfg wifi=0)"); return; }
    extern Thread *thread_create(void (*entry)(void *), void *arg);
    thread_create(wifi_thread, NULL);
}
