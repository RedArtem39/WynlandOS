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
 * Step 2: commands to the firmware (the command queue: a TFD per command,
 * the reply matched by its slot), the NVM read (MAC address, bands,
 * antennas, channels), the radio configured as iwx_init_hw does, then one
 * passive scan of every channel; the beacons heard are listed in the log.
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
#define TLV_API_CHANGES_SET         29
#define TLV_ENABLED_CAPABILITIES    30
#define TLV_N_SCAN_CHANNELS         31
#define TLV_CMD_VERSIONS            48
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
static Dma g_cmdbuf[TX_RING];                   /* one 4K buffer per command slot */
static uint32_t g_api[8], g_capa[8];            /* the firmware's API / capability bits */
static uint32_t g_n_scan_channels = 67;
static uint8_t g_cmdver[256][4];                /* cmd, group, cmd_ver, notif_ver */
static int g_ncmdver;
static inline bool has_api(int b) { return (g_api[b / 32] >> (b % 32)) & 1; }
static inline bool has_capa(int b) { return (g_capa[b / 32] >> (b % 32)) & 1; }
static int cmd_ver(uint8_t grp, uint8_t cmd)
{
    for (int i = 0; i < g_ncmdver; i++)
        if (g_cmdver[i][1] == grp && g_cmdver[i][0] == cmd) return g_cmdver[i][2];
    return 99;                                  /* IWX_FW_CMD_VER_UNKNOWN */
}

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
        } else if ((type == TLV_API_CHANGES_SET || type == TLV_ENABLED_CAPABILITIES) && tl == 8) {
            uint32_t idx = ((const uint32_t *)d)[0], bits = ((const uint32_t *)d)[1];
            if (idx < 8) (type == TLV_API_CHANGES_SET ? g_api : g_capa)[idx] |= bits;
        } else if (type == TLV_N_SCAN_CHANNELS && tl == 4) {
            g_n_scan_channels = *(const uint32_t *)d;
        } else if (type == TLV_CMD_VERSIONS) {
            g_ncmdver = 0;
            for (uint32_t o = 0; o + 4 <= tl && g_ncmdver < 256; o += 4, g_ncmdver++)
                memcpy(g_cmdver[g_ncmdver], d + o, 4);
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
    for (int i = 0; i < TX_RING; i++)
        if (!dma_alloc(&g_cmdbuf[i], 4096)) { say("no memory for command buffer ", i); return false; }
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

/* ---- a line for the log, built in a buffer ----------------------------- */
static char *put_s(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_u(char *p, uint32_t v)
{
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}
static char *put_i(char *p, int v) { if (v < 0) { *p++ = '-'; v = -v; } return put_u(p, (uint32_t)v); }
static char *put_x2(char *p, uint8_t v)
{
    static const char h[] = "0123456789abcdef";
    *p++ = h[v >> 4];
    *p++ = h[v & 15];
    return p;
}
static char *put_pad(char *p, char *start, int width) { while (p - start < width) *p++ = ' '; return p; }
static void put_line(char *b, char *end) { *end = 0; serial_write_string(b); serial_write_string("\r\n"); }
static inline uint16_t ld16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t ld32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static bool same6(const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 6; i++) if (a[i] != b[i]) return false;
    return true;
}

/* ---- the RX queue: what the firmware says ---------------------------- */
#define HBUS_TARG_MEM_RADDR         0x40c
#define HBUS_TARG_MEM_RDAT          0x41c
#define HBUS_TARG_WRPTR             0x460
#define FW_ADDR_CACHE_CONTROL       0xC0000000u
#define CMD_FAILED_MSK              0x40
#define GRP_LONG                    0x1
#define GRP_SYSTEM                  0x2
#define GRP_PHY_OPS                 0x4
#define GRP_DATA_PATH               0x5
#define GRP_NVM                     0xc
#define WIDE(g, c)                  (((uint32_t)(g) << 8) | (c))

static bool g_alive_irq, g_alive_ok, g_fw_error, g_init_complete, g_scan_done;
static uint32_t g_lmac_err, g_umac_err;
static int g_wait_idx = -1;                     /* the command slot waited for */
static bool g_cmd_done;
static uint8_t g_resp[4096];                    /* its reply's payload */
static uint32_t g_resp_len;
static uint8_t g_resp_flags;
static int g_tx_cur;
static int g_unknown_said;

/* the networks seen */
typedef struct { uint8_t bssid[6]; char ssid[33]; uint8_t ch; int rssi; const char *sec; } Bss;
#define MAX_BSS 64
static Bss g_bss[MAX_BSS];
static int g_nbss;
static uint32_t g_frames;

/* a beacon or a probe response: the network into the table */
static void handle_frame(const uint8_t *p, uint32_t plen)
{
    if (plen < 48) return;                      /* struct iwx_rx_mpdu_desc, v1 (22000) */
    uint32_t flen = ld16(p), status = ld32(p + 12);
    uint8_t flags2 = p[3], ea = p[32], eb = p[33], chan = p[34];
    if (!(status & 1) || !(status & 2)) return; /* CRC_OK, OVERRUN_OK */
    if (flen < 24 + 12 || 48 + flen > plen) return;
    const uint8_t *f = p + 48;
    g_frames++;
    if (f[0] != 0x80 && f[0] != 0x50) return;   /* beacon, probe response */
    uint32_t body = 24 + ((flags2 & 0x20) ? 2 : 0);   /* (the firmware's pad after the header) */
    if (body + 12 > flen) return;
    uint16_t cap = ld16(f + body + 10);
    int a = ea ? -(int)ea : -256, b = eb ? -(int)eb : -256;
    int rssi = a > b ? a : b;
    char ssid[33];
    int sl = -1;
    uint8_t ds = 0;
    const char *sec = (cap & 0x10) ? "WEP" : "open";
    bool rsn = false;
    for (uint32_t o = body + 12; o + 2 <= flen;) {
        uint8_t id = f[o], l = f[o + 1];
        if (o + 2 + l > flen) break;
        const uint8_t *e = f + o + 2;
        if (id == 0 && sl < 0 && l <= 32) {
            sl = l;
            for (int i = 0; i < l; i++) ssid[i] = (e[i] < 0x20 || e[i] == 0x7f) ? '?' : (char)e[i];
        } else if (id == 3 && l == 1) {
            ds = e[0];
        } else if (id == 48 && l >= 8) {        /* RSN: WPA2, or WPA3 when SAE is there */
            rsn = true;
            sec = "WPA2";
            uint32_t q = 8 + 4 * (uint32_t)ld16(e + 6);
            if (q + 2 <= l) {
                uint16_t nakm = ld16(e + q);
                bool sae = false, psk = false, eap = false;
                for (uint32_t k = 0; k < nakm && q + 2 + 4 * k + 4 <= l; k++) {
                    const uint8_t *s = e + q + 2 + 4 * k;
                    if (s[0] != 0x00 || s[1] != 0x0f || s[2] != 0xac) continue;
                    if (s[3] == 8) sae = true;
                    else if (s[3] == 2 || s[3] == 6) psk = true;
                    else if (s[3] == 1 || s[3] == 5) eap = true;
                }
                if (sae && psk) sec = "WPA2/3";
                else if (sae) sec = "WPA3";
                else if (eap && !psk) sec = "WPA2-EAP";
            }
        } else if (id == 221 && l >= 4 && !rsn && e[0] == 0x00 && e[1] == 0x50 && e[2] == 0xf2 && e[3] == 1) {
            sec = "WPA";
        }
        o += 2 + l;
    }
    if (sl < 0) sl = 0;
    ssid[sl] = 0;
    const uint8_t *bssid = f + 16;
    int i;
    for (i = 0; i < g_nbss; i++)
        if (same6(g_bss[i].bssid, bssid)) break;
    if (i == g_nbss) {
        if (g_nbss >= MAX_BSS) return;
        g_nbss++;
        memcpy(g_bss[i].bssid, bssid, 6);
        g_bss[i].rssi = -999;
    }
    Bss *n = &g_bss[i];
    memcpy(n->ssid, ssid, (size_t)sl + 1);
    n->ch = ds ? ds : chan;
    n->sec = sec;
    if (rssi > n->rssi) n->rssi = rssi;
}

static void say_notif(uint8_t grp, uint8_t code, uint32_t plen)
{
    if (g_unknown_said++ >= 24) return;
    char b[96], *q = put_s(b, "wifi: notification group 0x");
    q = put_x2(q, grp);
    q = put_s(q, " code 0x");
    q = put_x2(q, code);
    q = put_s(q, " length ");
    q = put_u(q, plen);
    put_line(b, q);
}

static void handle_pkt(uint8_t grp, uint8_t code, uint8_t idx, uint8_t qid, const uint8_t *p, uint32_t plen)
{
    if (qid == 0 && idx == g_wait_idx && !g_cmd_done) {    /* the reply to our command */
        uint32_t n = plen > sizeof g_resp ? sizeof g_resp : plen;
        memcpy(g_resp, p, n);
        g_resp_len = n;
        g_resp_flags = grp;
        g_cmd_done = true;
        return;
    }
    if ((grp & 0x3f) > GRP_LONG) { say_notif(grp, code, plen); return; }
    switch (code) {
    case CMD_ALIVE:
        if (plen >= 2) {
            uint16_t status = ld16(p);
            say("ALIVE notification, status ", status);
            if (status == ALIVE_STATUS_OK) g_alive_ok = true;
        }
        if (plen >= 112) {                      /* alive_resp v4..v6: the error tables */
            g_lmac_err = ld32(p + 20) & ~FW_ADDR_CACHE_CONTROL;
            g_umac_err = ld32(p + 108) & ~FW_ADDR_CACHE_CONTROL;
        }
        break;
    case 0x04:                                  /* INIT_COMPLETE_NOTIF */
        g_init_complete = true;
        break;
    case 0xc1:                                  /* REPLY_RX_MPDU_CMD */
        handle_frame(p, plen);
        break;
    case 0x0f:                                  /* SCAN_COMPLETE_UMAC */
    case 0xb5:                                  /* SCAN_ITERATION_COMPLETE_UMAC */
        if (!g_scan_done)
            say(code == 0x0f ? "scan complete, status " : "scan iteration complete, status ",
                plen >= 7 ? p[code == 0x0f ? 6 : 5] : 0xff);
        g_scan_done = true;
        break;
    case 0x02:                                  /* REPLY_ERROR */
        if (plen >= 5) {
            say("FIRMWARE REPLY_ERROR, type ", ld32(p));
            say("  for command ", p[4]);
        }
        break;
    case 0xb1: case 0xf7: case 0x9d: case 0xc9:
        break;                                  /* MFUART, debug log, statistics, MCC from the chub */
    default:
        say_notif(grp, code, plen);
    }
}

static void rx_drain(void)
{
    uint16_t hw = *(volatile uint16_t *)g_rx_stat.virt & 0xfff;
    hw &= RX_RING - 1;
    if (hw == g_rx_cur) return;
    int seen = 0;
    while (g_rx_cur != hw && seen++ < RX_RING) {
        const uint8_t *b = (const uint8_t *)g_rxbuf[g_rx_cur].virt;
        uint32_t off = 0;
        while (off + 8 < RBUF_SIZE) {
            uint32_t lnf = ld32(b + off);
            uint8_t code = b[off + 4], grp = b[off + 5], idx = b[off + 6], qid = b[off + 7];
            if ((qid == 0 && idx == 0 && code == 0 && grp == 0) || lnf == FH_FRAME_INVALID) break;
            uint32_t len = 4 + (lnf & FH_FRAME_SIZE_MSK);
            if (len < 8 || len > RBUF_SIZE - off) break;
            handle_pkt(grp, code, idx, qid, b + off + 8, len - 8);
            off += (len + FH_FRAME_ALIGN - 1) & ~(uint32_t)(FH_FRAME_ALIGN - 1);
        }
        g_rx_cur = (g_rx_cur + 1) % RX_RING;
    }
    /* what has been read, back to the card (aligned by 8, as iwx does) */
    uint16_t back = hw == 0 ? RX_RING - 1 : hw - 1;
    wr(RFH_Q0_FRBDCB_WIDX_TRG, back & ~7u);
}

/* the firmware's error tables (SRAM), after a firmware error */
static void dump_table(const char *name, uint32_t base)
{
    if (!base || !nic_lock()) return;
    say(name, base);
    for (int i = 0; i < 12; i++) {
        wr(HBUS_TARG_MEM_RADDR, base + 4 * (uint32_t)i);
        uint32_t v = rd(HBUS_TARG_MEM_RDAT);
        char b[48], *q = put_s(b, "  word ");
        q = put_u(q, (uint32_t)i);
        q = put_s(q, ": ");
        *q = 0;
        say(b, v);
    }
    nic_unlock();
}

/* the cause register, polled: ALIVE, errors; then the RX queue */
static void service(void)
{
    uint32_t r1 = rd(CSR_INT);
    if (r1 == 0xffffffffu || (r1 & 0xfffffff0u) == 0xa5a5a5a0u) {
        if (!g_fw_error) say("the card is gone, CSR_INT ", r1);
        g_fw_error = true;
        return;
    }
    uint32_t r2 = rd(CSR_FH_INT_STATUS);
    if (r1 || r2) {
        wr(CSR_INT, r1);
        wr(CSR_FH_INT_STATUS, r2);
    }
    if (r1 & INT_ALIVE) {
        step("ALIVE interrupt: the firmware runs");
        g_alive_irq = true;
        /* the firmware has set the RX queue up: the buffers to it */
        uint64_t *desc = (uint64_t *)g_rx_free.virt;
        for (int i = 0; i < RX_RING; i++) desc[i] = g_rxbuf[i].phys | (uint64_t)(i & 0x0fff);
        __asm__ volatile("mfence" ::: "memory");
        wr(RFH_Q0_FRBDCB_WIDX_TRG, 8);
    }
    if (g_alive_irq) rx_drain();
    if ((r1 & (INT_SW_ERR | INT_HW_ERR)) && !g_fw_error) {
        say("FIRMWARE / HARDWARE ERROR, CSR_INT ", r1);
        g_fw_error = true;
        dump_table("LMAC error table at ", g_lmac_err);
        dump_table("UMAC error table at ", g_umac_err);
        sticklog_panic_flush();
    }
    if (r1 & INT_RF_KILL) say("RF-kill switch changed, GP_CNTRL ", rd(CSR_GP_CNTRL));
}

static void poll_causes(uint64_t until)
{
    while (timer_get_ms() < until && !g_alive_ok && !g_fw_error) {
        service();
        sched_sleep_ms(1);
    }
}

/* ---- commands (iwx_send_cmd): a TFD on the command queue, the reply waited for */
static bool send_cmd(const char *what, uint32_t id, const void *data, uint32_t len)
{
    if (g_fw_error) return false;
    uint8_t op = id & 0xff, grp = (id >> 8) & 0xff, ver = (id >> 16) & 0xff;
    if (grp == 0) grp = GRP_LONG;               /* API >= 50: the old commands in the LONG group */
    if (len > 4096 - 8) return false;
    int idx = g_tx_cur;
    uint8_t *c = (uint8_t *)g_cmdbuf[idx].virt;
    c[0] = op; c[1] = grp; c[2] = (uint8_t)idx; c[3] = 0;      /* struct iwx_cmd_header_wide */
    c[4] = (uint8_t)len; c[5] = (uint8_t)(len >> 8); c[6] = 0; c[7] = ver;
    if (len) memcpy(c + 8, data, len);
    uint8_t *t = (uint8_t *)g_txq.virt + idx * TFD_SIZE;       /* struct iwx_tfh_tfd */
    memset(t, 0, TFD_SIZE);
    uint32_t tot = 8 + len;
    uint16_t l0 = (uint16_t)(tot < 20 ? tot : 20), n = 1;
    uint64_t a = g_cmdbuf[idx].phys;
    memcpy(t + 2, &l0, 2);
    memcpy(t + 4, &a, 8);
    if (tot > 20) {
        uint16_t l1 = (uint16_t)(tot - 20);
        uint64_t a1 = a + 20;
        memcpy(t + 12, &l1, 2);
        memcpy(t + 14, &a1, 8);
        n = 2;
    }
    memcpy(t, &n, 2);
    __asm__ volatile("mfence" ::: "memory");
    g_cmd_done = false;
    g_resp_len = 0;
    g_resp_flags = 0;
    g_wait_idx = idx;
    g_tx_cur = (g_tx_cur + 1) % TX_RING;
    wr(HBUS_TARG_WRPTR, (uint32_t)g_tx_cur);    /* queue 0 << 16 | the write pointer */
    uint64_t until = timer_get_ms() + 1500;
    while (!g_cmd_done && !g_fw_error && timer_get_ms() < until) {
        service();
        if (!g_cmd_done) sched_sleep_ms(1);
    }
    g_wait_idx = -1;
    if (!g_cmd_done) {
        sayx(g_fw_error ? "command failed (firmware error):" : "NO REPLY to the command:");
        sayx(what);
        sticklog_panic_flush();
        return false;
    }
    if (g_resp_flags & CMD_FAILED_MSK) { sayx("the firmware REFUSED the command:"); sayx(what); return false; }
    step(what);
    return true;
}

/* ---- the radio up (iwx_run_init_mvm_ucode, iwx_nvm_get, iwx_init_hw) -- */
static const uint8_t nvm_channels[51] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92,
    96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165, 169, 173, 177, 181
};
typedef struct { uint8_t num, band; bool active; } Chan;
static Chan g_chan[64];
static int g_nchan;
static uint8_t g_mac[6];
static bool g_band5, g_lar;
static uint8_t g_tx_ant, g_rx_ant;

static void channel_map(const uint8_t *prof, uint32_t n, bool v4)
{
    g_nchan = 0;
    for (uint32_t i = 0; i < n && i < 51 && g_nchan < 64; i++) {
        uint32_t fl = v4 ? ld32(prof + 4 * i) : ld16(prof + 2 * i);
        bool is5 = i >= 14;
        if ((is5 && !g_band5) || !(fl & 1)) continue;        /* NVM_CHANNEL_VALID */
        g_chan[g_nchan].num = nvm_channels[i];
        g_chan[g_nchan].band = is5 ? 0 : 1;                   /* PHY_BAND_5 / _24 */
        g_chan[g_nchan].active = (fl & 8) != 0;               /* NVM_CHANNEL_ACTIVE */
        g_nchan++;
    }
    char b[220], *q = put_s(b, "wifi: channels ");
    q = put_u(q, (uint32_t)g_nchan);
    q = put_s(q, ":");
    for (int i = 0; i < g_nchan && q < b + 210; i++) { *q++ = ' '; q = put_u(q, g_chan[i].num); }
    put_line(b, q);
}

static bool read_nvm(void)
{
    uint32_t zero = 0;
    if (!send_cmd("NVM_GET_INFO", WIDE(GRP_NVM, 0x02), &zero, 4)) return false;
    bool v4 = has_api(48);                      /* REGULATORY_NVM_INFO */
    uint32_t want = v4 ? 468 : 128;
    if (g_resp_len != want) { say("NVM reply of an unexpected length ", g_resp_len); return false; }
    const uint8_t *r = g_resp;
    uint32_t mac_flags = ld32(r + 8);
    g_band5 = (mac_flags & 2) != 0;
    uint8_t ntx = (uint8_t)ld32(r + 12), nrx = (uint8_t)ld32(r + 16);
    g_lar = ld32(r + 20) && has_capa(1);        /* LAR_SUPPORT */
    say("NVM version ", ld16(r + 4));
    say("  SKU flags (1 2.4 GHz, 2 5 GHz, 4 11n, 8 11ac, 0x10 11ax) ", mac_flags);
    say("  antennas TX ", ntx);
    say("  antennas RX ", nrx);
    say("  location-aware regulatory ", g_lar);
    g_tx_ant = (uint8_t)((g_phy_config >> 16) & 0xf);
    g_rx_ant = (uint8_t)((g_phy_config >> 20) & 0xf);
    if (ntx) g_tx_ant &= ntx;
    if (nrx) g_rx_ant &= nrx;
    if (v4) channel_map(r + 28, ld32(r + 24), true);
    else channel_map(r + 24, 51, false);

    /* the MAC address: the OEM's strap, else the OTP (iwx_set_mac_addr_from_csr) */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t m0 = rd(0x380 + (pass ? 0x00 : 0x08)), m1 = rd(0x380 + (pass ? 0x04 : 0x0c));
        const uint8_t *h0 = (const uint8_t *)&m0, *h1 = (const uint8_t *)&m1;
        g_mac[0] = h0[3]; g_mac[1] = h0[2]; g_mac[2] = h0[1]; g_mac[3] = h0[0];
        g_mac[4] = h1[1]; g_mac[5] = h1[0];
        bool zero6 = true, ones = true;
        for (int i = 0; i < 6; i++) { if (g_mac[i]) zero6 = false; if (g_mac[i] != 0xff) ones = false; }
        static const uint8_t reserved[6] = { 0x02, 0xcc, 0xaa, 0xff, 0xee, 0x00 };
        if (!zero6 && !ones && !same6(g_mac, reserved) && !(g_mac[0] & 1)) break;
    }
    char b[64], *q = put_s(b, "wifi: MAC address ");
    for (int i = 0; i < 6; i++) { if (i) *q++ = ':'; q = put_x2(q, g_mac[i]); }
    put_line(b, q);
    return true;
}

static bool update_mcc(void)
{
    uint8_t cmd[28];
    memset(cmd, 0, sizeof cmd);
    uint16_t mcc = ('Z' << 8) | 'Z';            /* "ZZ": the NVM's own profile */
    memcpy(cmd, &mcc, 2);
    cmd[2] = (has_api(9) || has_capa(29)) ? 0x10 : 0;   /* MCC_SOURCE_GET_CURRENT : OLD_FW */
    if (!send_cmd("MCC_UPDATE (ZZ)", 0xc8, cmd, sizeof cmd)) return false;
    if (g_resp_len < 20) return true;
    uint32_t n = ld32(g_resp + 16);
    char b[64], *q = put_s(b, "wifi: regulatory domain ");
    *q++ = (char)g_resp[5];
    *q++ = (char)g_resp[4];
    put_line(b, q);
    if (g_resp_len == 20 + 4 * n) channel_map(g_resp + 20, n, true);
    return true;
}

static bool radio_up(void)
{
    uint32_t v;
    /* (the card stays awake: the access request held from here) */
    if (!nic_lock()) return false;
    v = 2;                                      /* IWX_INIT_NVM */
    if (!send_cmd("INIT_EXTENDED_CFG", WIDE(GRP_SYSTEM, 0x03), &v, 4)) return false;
    v = 0;
    if (!send_cmd("NVM_ACCESS_COMPLETE", WIDE(GRP_NVM, 0x00), &v, 4)) return false;
    for (uint64_t until = timer_get_ms() + 2000; !g_init_complete && !g_fw_error && timer_get_ms() < until;) {
        service();
        sched_sleep_ms(1);
    }
    if (!g_init_complete) { sayx("NO INIT_COMPLETE from the firmware"); sticklog_panic_flush(); return false; }
    step("init complete");
    if (!read_nvm()) return false;

    v = g_tx_ant;
    if (!send_cmd("TX_ANT_CONFIGURATION", 0x98, &v, 4)) return false;
    uint32_t bt[2] = { 3, 0 };                  /* BT_COEX_WIFI */
    if (!send_cmd("BT_CONFIG", 0x9b, bt, 8)) return false;
    /* integrated (CNVi), the long-latency crystal of Tiger Lake's (Linux:
       iwl_qu_long_latency_trans_cfg for 0xA0F0) */
    int scanver = cmd_ver(GRP_LONG, 0x0d);
    uint32_t soc[2] = { (2u << 2) | ((scanver != 99 && scanver >= 2) ? 2u : 0u), 12000 };
    if (!send_cmd("SOC_CONFIGURATION", WIDE(GRP_SYSTEM, 0x01), soc, 8)) return false;
    if (has_capa(12)) {                         /* DQA_SUPPORT */
        v = 0;
        if (!send_cmd("DQA_ENABLE", WIDE(GRP_DATA_PATH, 0x00), &v, 4)) return false;
    }
    if (has_capa(74)) {                         /* CT_KILL_BY_FW: the firmware's thermal duty */
        uint8_t th[20];
        memset(th, 0, sizeof th);
        if (!send_cmd("TEMP_REPORTING_THRESHOLDS", WIDE(GRP_PHY_OPS, 0x04), th, sizeof th)) return false;
    }
    v = 0;                                      /* device power: no power save */
    if (!send_cmd("POWER_TABLE", 0x77, &v, 4)) return false;
    if (g_lar && !update_mcc()) return false;
    if (!has_api(56)) { sayx("the firmware has no reduced scan config"); return false; }
    uint8_t sc[12];
    memset(sc, 0, sizeof sc);
    int cfgver = cmd_ver(GRP_LONG, 0x0c);
    if (cfgver == 99 || cfgver < 5) sc[2] = 0xff;
    sc[4] = g_tx_ant;
    sc[8] = g_rx_ant;
    if (!send_cmd("SCAN_CONFIG", WIDE(GRP_LONG, 0x0c), sc, sizeof sc)) return false;
    uint8_t bf[60];
    memset(bf, 0, sizeof bf);
    if (!send_cmd("BEACON_FILTERING (off)", 0xd2, bf, sizeof bf)) return false;
    return true;
}

/* ---- the scan (iwx_umac_scan_v14 / _v17): passive, every channel ------ */
static uint8_t g_scancmd[1940];

static void fill_probe(uint8_t *pp)
{
    /* struct iwx_scan_probe_req: five segments, then the frame */
    uint8_t *f = pp + 20, *q = f;
    *q++ = 0x40; *q++ = 0;                      /* probe request */
    q += 2;
    for (int i = 0; i < 6; i++) *q++ = 0xff;
    memcpy(q, g_mac, 6);
    q += 6;
    for (int i = 0; i < 6; i++) *q++ = 0xff;
    q += 2;
    *q++ = 0; *q++ = 0;                         /* the SSID (the firmware's) */
    uint16_t v = (uint16_t)(q - f), start;
    memcpy(pp + 2, &v, 2);                      /* mac_header: offset 0, this long */
    start = v;
    memcpy(pp + 4, &start, 2);                  /* band_data[0] (2.4 GHz) */
    static const uint8_t r24[] = { 1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 50, 4, 0x30, 0x48, 0x60, 0x6c };
    memcpy(q, r24, sizeof r24);
    q += sizeof r24;
    if (has_capa(9)) { *q++ = 3; *q++ = 1; *q++ = 0; }     /* DS_PARAM_SET_IE_SUPPORT */
    v = (uint16_t)((q - f) - start);
    memcpy(pp + 6, &v, 2);
    if (g_band5) {
        static const uint8_t r5[] = { 1, 8, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c };
        v = (uint16_t)(q - f);
        memcpy(pp + 8, &v, 2);
        memcpy(q, r5, sizeof r5);
        q += sizeof r5;
        v = sizeof r5;
        memcpy(pp + 10, &v, 2);
    }
    v = (uint16_t)(q - f);
    memcpy(pp + 16, &v, 2);                     /* common_data: empty */
}

static bool start_scan(void)
{
    int ver = cmd_ver(GRP_LONG, 0x0d);
    say("scan command version ", (uint64_t)ver);
    uint8_t *c = g_scancmd;
    memset(c, 0, sizeof g_scancmd);
    uint32_t prio = 6;                          /* SCAN_PRIORITY_EXT_6 */
    memcpy(c + 4, &prio, 4);
    uint8_t *g = c + 8;                         /* general params v10 / v11 */
    uint16_t flags = (1u << 11) | (1u << 1) | (1u << 2) | (1u << 7);   /* FORCE_PASSIVE PASS_ALL NTFY_ITER ADAPTIVE_DWELL */
    memcpy(g, &flags, 2);
    g[4] = 10; g[5] = 10;                       /* active dwell */
    g[6] = 2; g[7] = 8; g[8] = 10;              /* adaptive dwell: APs on 2.4, 5, social */
    uint16_t budget = 300;
    memcpy(g + 10, &budget, 2);
    memcpy(g + 28, &prio, 4);
    g[32] = 110; g[33] = 110;                   /* passive dwell */
    uint8_t *ch = c + 44;                       /* channel params v6 / v7 */
    ch[0] = 1u << 5;                            /* ENABLE_CHAN_ORDER */
    ch[2] = 10; ch[3] = 2;
    int n = 0;
    for (int i = 0; i < g_nchan && n < 67 && (uint32_t)n < g_n_scan_channels; i++, n++) {
        uint8_t *e = ch + 4 + 8 * n;
        if (ver == 17) {
            uint32_t fl = (uint32_t)g_chan[i].band << 30;
            memcpy(e, &fl, 4);
            e[4] = g_chan[i].num; e[5] = 0x80; e[6] = 1; e[7] = 0;
        } else if (has_api(58)) {               /* SCAN_EXT_CHAN_VER */
            e[4] = g_chan[i].num; e[5] = g_chan[i].band; e[6] = 1; e[7] = 0;
        } else {
            e[4] = g_chan[i].num; e[5] = 1; e[6] = 0; e[7] = 0;
        }
    }
    ch[1] = (uint8_t)n;
    uint8_t *per = c + 584;                     /* periodic: one iteration */
    per[2] = 1;
    fill_probe(c + 596);
    g_scan_done = false;
    g_nbss = 0;
    g_frames = 0;
    say("scanning channels: ", (uint64_t)n);
    return send_cmd("SCAN_REQ_UMAC", WIDE(GRP_LONG, 0x0d), c, sizeof g_scancmd);
}

static void list_networks(void)
{
    /* by signal, the strongest first */
    for (int i = 1; i < g_nbss; i++)
        for (int j = i; j > 0 && g_bss[j].rssi > g_bss[j - 1].rssi; j--) {
            Bss t = g_bss[j]; g_bss[j] = g_bss[j - 1]; g_bss[j - 1] = t;
        }
    say("frames received ", g_frames);
    say("NETWORKS FOUND: ", (uint64_t)g_nbss);
    for (int i = 0; i < g_nbss; i++) {
        Bss *n = &g_bss[i];
        char b[160], *q = put_s(b, "wifi:   "), *s = q;
        q = put_i(q, n->rssi);
        q = put_s(q, " dBm");
        q = put_pad(q, s, 9);
        s = q;
        q = put_s(q, "ch ");
        q = put_u(q, n->ch);
        q = put_pad(q, s, 7);
        s = q;
        q = put_s(q, n->sec);
        q = put_pad(q, s, 10);
        for (int k = 0; k < 6; k++) { if (k) *q++ = ':'; q = put_x2(q, n->bssid[k]); }
        q = put_s(q, "  ");
        q = put_s(q, n->ssid[0] ? n->ssid : "(hidden)");
        put_line(b, q);
    }
    sticklog_panic_flush();
}

static void wifi_up(void)
{
    say("firmware API bits ", ((uint64_t)g_api[1] << 32) | g_api[0]);
    say("firmware capability bits ", ((uint64_t)g_capa[1] << 32) | g_capa[0]);
    say("  more capability bits ", ((uint64_t)g_capa[3] << 32) | g_capa[2]);
    say("command versions ", (uint64_t)g_ncmdver);
    say("scan channels at most ", g_n_scan_channels);
    if (!radio_up()) { step("the radio did not come up -- step 2 stops here"); return; }
    step("RADIO UP");
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!start_scan()) return;
        for (uint64_t until = timer_get_ms() + 20000; !g_scan_done && !g_fw_error && timer_get_ms() < until;) {
            service();
            sched_sleep_ms(2);
        }
        if (!g_scan_done) sayx("the scan did not finish in 20 s");
        list_networks();
        if (g_nbss || g_fw_error) break;
        sched_sleep_ms(2000);
    }
    step(g_nbss ? "SCAN DONE -- step 2 done" : "the scan found nothing");
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
    if (g_alive_ok) { step("FIRMWARE ALIVE -- step 1 done"); wifi_up(); }
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
