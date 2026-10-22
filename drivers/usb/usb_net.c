/*
 * WynlandOS - USB networking: a phone's USB tethering (RNDIS: Android) or
 * a CDC Ethernet (ECM) adapter or modem -- a stopgap for a machine whose
 * own network (Wi-Fi) has no driver yet.
 *
 * Bulk IN keeps USBNET_RX buffers queued: each completion (drain_locked,
 * the USB lock held) splits it into frames (RNDIS: one PACKET_MSG or
 * more; ECM: one frame) into a small ring that net_poll() empties. Bulk
 * OUT: a ring of frame buffers, sent without waiting; a completion frees
 * the oldest. One device at a time; unplugged, the network goes down and
 * comes back with the next one (kernel/main.c, boot_network).
 */
#include "usb_internal.h"
#include <wynland/net.h>
#include <wynland/pmm.h>

#define USBNET_RX       4
#define USBNET_RXBYTES  16384u          /* the transfer size we tell RNDIS we take */
#define USBNET_TX       16
#define USBNET_TXBYTES  2048u
#define USBNET_Q        64              /* frames waiting for net_poll() */
#define USBNET_MTU      1514u

#define RNDIS_PACKET    0x00000001u
#define RNDIS_INIT      0x00000002u
#define RNDIS_QUERY     0x00000004u
#define RNDIS_SET       0x00000005u
#define OID_MAC         0x01010101u     /* OID_802_3_PERMANENT_ADDRESS */
#define OID_FILTER      0x0001010Eu     /* OID_GEN_CURRENT_PACKET_FILTER */

static struct {
    UsbDev  *d;
    int      kind;                      /* 1 RNDIS, 2 ECM */
    uint8_t  comm_iface;
    uint8_t  in_id, out_id;
    UsbRing  rin, rout;
    uint8_t *rx[USBNET_RX];  uint64_t rx_phys[USBNET_RX];
    uint32_t rx_next;
    uint8_t *tx[USBNET_TX];  uint64_t tx_phys[USBNET_TX];
    uint32_t tx_head, tx_busy;
    uint8_t  mac[6];
    uint32_t reqid;
} g_un;

static volatile bool g_up;
static uint8_t  g_q[USBNET_Q][USBNET_MTU];
static uint16_t g_qlen[USBNET_Q];
static volatile uint32_t g_qhead, g_qtail;

static inline uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void wr32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void q_push(const uint8_t *f, uint32_t len)
{
    if (len < 14 || len > USBNET_MTU) return;
    uint32_t h = g_qhead;
    if (h - g_qtail >= USBNET_Q) return;          /* full: dropped, as a NIC would */
    memcpy(g_q[h % USBNET_Q], f, len);
    g_qlen[h % USBNET_Q] = (uint16_t)len;
    __sync_synchronize();
    g_qhead = h + 1;
}

static void rx_submit(uint32_t i)
{
    usb_ring_push(&g_un.rin, g_un.rx_phys[i], USBNET_RXBYTES, (1u << 10) | (1u << 5));
}

/* a transfer event of ours (drain_locked, the USB lock held) */
bool usb_net_event(uint8_t slot, uint8_t ep, uint32_t code, uint32_t resid)
{
    if (!g_un.d || slot != g_un.d->slot) return false;
    if (ep == g_un.out_id) {
        if (g_un.tx_busy) g_un.tx_busy--;
        return true;
    }
    if (ep != g_un.in_id) return false;
    uint32_t i = g_un.rx_next;
    g_un.rx_next = (i + 1) % USBNET_RX;
    if (code == 1 || code == 13) {
        uint32_t len = resid <= USBNET_RXBYTES ? USBNET_RXBYTES - resid : 0;
        const uint8_t *b = g_un.rx[i];
        if (g_un.kind == 2) {
            q_push(b, len);
        } else {
            /* one or more PACKET_MSGs: type, length, data offset (from byte 8), data length */
            for (uint32_t off = 0; off + 44 <= len; ) {
                uint32_t type = rd32le(b + off), mlen = rd32le(b + off + 4);
                if (type != RNDIS_PACKET || mlen < 44 || off + mlen > len) break;
                uint32_t doff = rd32le(b + off + 8), dlen = rd32le(b + off + 12);
                if (8 + doff + dlen <= mlen) q_push(b + off + 8 + doff, dlen);
                off += mlen;
            }
        }
    }
    rx_submit(i);
    usb_db(g_un.d->slot, g_un.in_id, 0);
    return true;
}

int usb_net_receive(void *buf, uint32_t max)
{
    uint32_t t = g_qtail;
    if (t == g_qhead) return 0;
    uint32_t len = g_qlen[t % USBNET_Q];
    if (len > max) len = max;
    memcpy(buf, g_q[t % USBNET_Q], len);
    __sync_synchronize();
    g_qtail = t + 1;
    return (int)len;
}

bool usb_net_send(const void *data, uint32_t len)
{
    if (!g_up || len > USBNET_MTU) return false;
    uint64_t fl = spin_lock_irqsave(&g_usb_lock);
    if (g_un.tx_busy >= USBNET_TX) { spin_unlock_irqrestore(&g_usb_lock, fl); return false; }
    uint32_t i = g_un.tx_head;
    g_un.tx_head = (i + 1) % USBNET_TX;
    uint8_t *b = g_un.tx[i];
    uint32_t n;
    if (g_un.kind == 1) {
        memset(b, 0, 44);
        wr32le(b, RNDIS_PACKET);
        wr32le(b + 8, 36);                         /* the data, from byte 8 */
        wr32le(b + 12, len);
        memcpy(b + 44, data, len);
        n = 44 + len;
        wr32le(b + 4, n);
    } else {
        memcpy(b, data, len);
        n = len;
    }
    /* a transfer of whole packets would need a zero-length one to end it:
       a byte more instead (RNDIS ignores it; Ethernet takes padding) */
    uint32_t maxp = g_un.d->speed >= 4 ? 1024 : g_un.d->speed == 3 ? 512 : 64;
    if (n % maxp == 0) b[n++] = 0;
    g_un.tx_busy++;
    usb_ring_push(&g_un.rout, g_un.tx_phys[i], n, (1u << 10) | (1u << 5));
    usb_db(g_un.d->slot, g_un.out_id, 0);
    spin_unlock_irqrestore(&g_usb_lock, fl);
    return true;
}

bool usb_net_ready(void) { return g_up; }
void usb_net_get_mac(uint8_t *mac) { memcpy(mac, g_un.mac, 6); }

/* the device is gone (port_scan): the network with it */
void usb_net_gone(UsbDev *d)
{
    if (g_un.d != d) return;
    g_up = false;
    g_un.d = NULL;
    usb_log("network device gone");
}

/* ---- RNDIS control: a message out, its answer back (encapsulated) ---- */
static uint8_t g_msg[512];

static bool rndis_call(UsbDev *d, uint8_t *msg, uint32_t len, uint32_t want)
{
    wr32le(msg + 8, ++g_un.reqid);
    if (!usb_control(d, 0x21, 0x00, 0, g_un.comm_iface, msg, (uint16_t)len, false)) {
        usb_log_hex("RNDIS: the device refused message type=", want);
        return false;
    }
    for (int i = 0; i < 100; i++) {
        memset(g_msg, 0, sizeof g_msg);
        if (usb_control(d, 0xA1, 0x01, 0, g_un.comm_iface, g_msg, sizeof g_msg, true) &&
            rd32le(g_msg) == (want | 0x80000000u) && rd32le(g_msg + 8) == g_un.reqid) {
            if (rd32le(g_msg + 12)) usb_log_hex("RNDIS: status=", rd32le(g_msg + 12));
            return rd32le(g_msg + 12) == 0;       /* status: success */
        }
        uint64_t t = __builtin_ia32_rdtsc();
        while (__builtin_ia32_rdtsc() - t < 20000000ULL) __asm__ volatile("pause");
    }
    return false;
}

static bool rndis_init(UsbDev *d)
{
    uint8_t m[64];
    memset(m, 0, sizeof m);
    wr32le(m, RNDIS_INIT); wr32le(m + 4, 24);
    wr32le(m + 12, 1); wr32le(m + 16, 0); wr32le(m + 20, USBNET_RXBYTES);
    if (!rndis_call(d, m, 24, RNDIS_INIT)) { usb_log("RNDIS: INITIALIZE failed"); return false; }

    memset(m, 0, sizeof m);
    /* with an (empty) buffer for the answer, as Linux sends it: some
       devices -- QEMU's -- refuse a query without one */
    wr32le(m, RNDIS_QUERY); wr32le(m + 4, 28 + 8); wr32le(m + 12, OID_MAC);
    wr32le(m + 16, 8); wr32le(m + 20, 20);
    if (!rndis_call(d, m, 28 + 8, RNDIS_QUERY)) { usb_log("RNDIS: no MAC address"); return false; }
    uint32_t ilen = rd32le(g_msg + 16), ioff = rd32le(g_msg + 20);
    if (ilen < 6 || 8 + ioff + 6 > sizeof g_msg) return false;
    memcpy(g_un.mac, g_msg + 8 + ioff, 6);

    memset(m, 0, sizeof m);
    wr32le(m, RNDIS_SET); wr32le(m + 4, 32); wr32le(m + 12, OID_FILTER);
    wr32le(m + 16, 4); wr32le(m + 20, 20);
    wr32le(m + 28, 0x0B);                         /* directed, multicast, broadcast */
    if (!rndis_call(d, m, 32, RNDIS_SET)) { usb_log("RNDIS: packet filter refused"); return false; }
    return true;
}

/* ECM: the MAC address is a string descriptor of 12 hex digits */
static bool ecm_mac(UsbDev *d, uint8_t str_index)
{
    uint8_t s[64];
    memset(s, 0, sizeof s);
    if (!str_index || !usb_control(d, 0x80, 6, (uint16_t)(0x0300 | str_index), 0x0409, s, sizeof s, true))
        return false;
    for (int i = 0; i < 12; i++) {
        uint8_t c = s[2 + 2 * i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return false;
        if (i & 1) g_un.mac[i / 2] |= (uint8_t)v; else g_un.mac[i / 2] = (uint8_t)(v << 4);
    }
    return true;
}

bool usb_net_setup(UsbDev *d, const UsbNetInfo *ni)
{
    if (g_un.d) { usb_log("a second network device: not used"); return false; }
    memset(&g_un, 0, sizeof g_un);
    g_un.kind = ni->kind;
    g_un.comm_iface = ni->comm_iface;
    g_un.in_id = (uint8_t)(((ni->in_addr & 0x0F) << 1) | 1u);
    g_un.out_id = (uint8_t)((ni->out_addr & 0x0F) << 1);

    if (ni->kind == 2) {
        /* the data interface's second setting is the one with endpoints */
        if (ni->data_alt && !usb_control(d, 0x01, 11, ni->data_alt, ni->data_iface, NULL, 0, false)) {
            usb_log("ECM: SET_INTERFACE failed");
            return false;
        }
        if (!ecm_mac(d, ni->mac_str)) {
            usb_log("ECM: no MAC address string; a made-up one");
            static const uint8_t mk[6] = { 0x02, 0x57, 0x59, 0x4E, 0x00, 0x01 };
            memcpy(g_un.mac, mk, 6);
        }
    } else if (!rndis_init(d)) {
        return false;
    }

    void *ri = usb_pages(1, &g_un.rin.phys), *ro = usb_pages(1, &g_un.rout.phys);
    uint64_t rxb_phys = 0, txb_phys = 0;
    /* 4 x 16 KB of receive buffers in one 64 KB-aligned window: a TRB's
       buffer must not cross a 64 KB line */
    uint8_t *rxb = usb_pages(32, &rxb_phys);
    uint8_t *txb = usb_pages(USBNET_TX * USBNET_TXBYTES / PAGE_SIZE, &txb_phys);
    if (!ri || !ro || !rxb || !txb) return false;
    uint64_t skip = (0x10000 - (rxb_phys & 0xFFFF)) & 0xFFFF;
    for (int i = 0; i < USBNET_RX; i++) {
        g_un.rx[i] = rxb + skip + (uint64_t)i * USBNET_RXBYTES;
        g_un.rx_phys[i] = rxb_phys + skip + (uint64_t)i * USBNET_RXBYTES;
    }
    for (int i = 0; i < USBNET_TX; i++) {
        g_un.tx[i] = txb + (uint64_t)i * USBNET_TXBYTES;
        g_un.tx_phys[i] = txb_phys + (uint64_t)i * USBNET_TXBYTES;
    }
    g_un.rin.trb = (UsbTrb *)ri;
    g_un.rout.trb = (UsbTrb *)ro;
    usb_ring_init(&g_un.rin, g_un.rin.phys);
    usb_ring_init(&g_un.rout, g_un.rout.phys);
    if (!usb_configure_ep(d, g_un.out_id, 2, ni->out_maxp, 0, &g_un.rout) ||
        !usb_configure_ep(d, g_un.in_id, 6, ni->in_maxp, 0, &g_un.rin)) {
        usb_log("network: endpoints not configured");
        return false;
    }

    g_qhead = g_qtail = 0;
    uint64_t fl = spin_lock_irqsave(&g_usb_lock);
    g_un.d = d;
    for (uint32_t i = 0; i < USBNET_RX; i++) rx_submit(i);
    usb_db(d->slot, g_un.in_id, 0);
    spin_unlock_irqrestore(&g_usb_lock, fl);
    g_up = true;
    usb_log(ni->kind == 1 ? "network: RNDIS (a phone's USB tethering) up" : "network: CDC Ethernet up");
    usb_log_hex("network: MAC (low 3 bytes)=", (uint64_t)g_un.mac[3] << 16 | (uint64_t)g_un.mac[4] << 8 | g_un.mac[5]);
    return true;
}
