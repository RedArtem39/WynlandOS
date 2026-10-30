/*
 * WynlandOS - xHCI (USB 3) host controller driver
 *
 * Polled, no MSI: one command ring, one event ring, one interrupter.
 * Enumeration is synchronous; HID reports are drained by a kernel thread,
 * mass storage transfers wait for their transfer event in place.
 */

#include "usb_internal.h"
#include <wynland/pci.h>
#include <wynland/vmm.h>
#include <wynland/pmm.h>
#include <wynland/memlayout.h>
#include <wynland/sched.h>

extern void serial_write_string(const char *s);
extern void uint_to_hex(uint64_t v, char *buf);

#define OP       g_usb_op
#define RT       g_usb_rt
#define DB       g_usb_db

#define R32(p)   (*(volatile uint32_t *)(p))
#define W32(p,v) (*(volatile uint32_t *)(p) = (uint32_t)(v))

#define USBSTS_HCH  (1u << 0)
#define USBSTS_CNR  (1u << 11)
#define USBCMD_RUN  (1u << 0)
#define USBCMD_HCRST (1u << 1)

#define PORTSC_CCS  (1u << 0)
#define PORTSC_PED  (1u << 1)
#define PORTSC_PR   (1u << 4)
#define PORTSC_PP   (1u << 9)
#define PORTSC_CSC  (1u << 17)
#define PORTSC_CHG  0x00FE0000u

UsbDev g_usb_devs[USB_MAX_DEVS];
UsbDev *g_usb_storage;
uint32_t g_usb_max_ports;
volatile uint8_t *g_usb_op;
volatile uint8_t *g_usb_rt;
volatile uint8_t *g_usb_db;
uint32_t g_usb_ctx_stride = 32;

static volatile uint8_t *g_mmio;
static UsbRing  g_cmd_ring;
static UsbRing  g_ev_ring;
static uint64_t *g_dcbaa;
static uint64_t  g_erst_phys;
static uint8_t  *g_cfg_buf;
static uint64_t  g_cfg_buf_phys;
static uint8_t  *g_ctl_buf;
static uint64_t  g_ctl_buf_phys;
Spinlock         g_usb_lock;
static bool      g_up;
static bool      g_usb_ac64 = true;

static volatile bool     g_cmd_done;
static volatile uint8_t  g_cmd_slot;
static volatile uint32_t g_cmd_code;

static volatile bool     g_ctl_done;
static volatile uint8_t  g_ctl_slot;
static volatile uint32_t g_ctl_code;

static volatile bool     g_bulk_done;
static volatile uint8_t  g_bulk_slot;
static volatile uint8_t  g_bulk_ep_in;
static volatile uint8_t  g_bulk_ep_out;
static volatile uint32_t g_bulk_code;

void usb_log(const char *s)
{
    serial_write_string("USB: ");
    serial_write_string(s);
    serial_write_string("\r\n");
}

void usb_log_hex(const char *s, uint64_t v)
{
    char b[20];
    serial_write_string("USB: ");
    serial_write_string(s);
    uint_to_hex(v, b);
    serial_write_string(b);
    serial_write_string("\r\n");
}

void *usb_pages(uint32_t count, uint64_t *phys)
{
    void *p = pmm_alloc_contiguous(count);
    if (!p) return NULL;
    void *v = P2V((uint64_t)(uintptr_t)p);
    memset(v, 0, (size_t)count * PAGE_SIZE);
    if (phys) {
        *phys = (uint64_t)(uintptr_t)p;
        if (!g_usb_ac64 && *phys >= 0x100000000ULL)
            usb_log("WARNING: xHCI has no 64-bit DMA but ring is above 4GB");
    }
    return v;
}

static inline uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint64_t usb_tsc(void)
{
    return __builtin_ia32_rdtsc();
}

static void usb_spin(void)
{
    __asm__ volatile("pause");
}

/* A wait that lets the rest of the machine run: the big kernel lock let
   go and the core given up once a millisecond. These waits used to spin
   with the lock held and interrupts off -- a phone's port reset (20-300
   ms) or a control transfer stopped the boot core's clock, and with it
   every key and frame (the stall watchdog caught usb_control). */
bool g_usb_no_relax;   /* a panic's flush (kernel/sticklog.c): no scheduler */
void usb_relax(uint64_t *last_ns)
{
    extern uint64_t sched_clock_ns(void);
    extern void sched_yield(void);
    if (g_usb_no_relax) return;
    uint64_t now = sched_clock_ns();
    if (now - *last_ns < 1000000ull) return;
    *last_ns = now;
    extern bool bkl_drop(void);
    extern void bkl_retake(bool had);
    bool had = bkl_drop();
    sched_yield();
    bkl_retake(had);
}

static void usb_delay_ms(uint32_t ms)
{
    extern uint64_t sched_clock_ns(void);
    uint64_t start = usb_tsc(), relaxed = sched_clock_ns();
    uint64_t limit = (uint64_t)ms * 5000000ULL;
    while (usb_tsc() - start < limit) {
        usb_spin();
        if (ms > 1) usb_relax(&relaxed);
    }
}

void usb_ring_init(UsbRing *r, uint64_t phys)
{
    r->phys = phys;
    r->enq = 0;
    r->ccs = 1;
    if (r->trb) {
        UsbTrb *l = &r->trb[USB_TRBS - 1];
        l->param = phys;
        l->status = 0;
        l->control = (6u << 10) | (1u << 1) | 1u;
    }
}

void usb_ring_push(UsbRing *r, uint64_t param, uint32_t status, uint32_t control)
{
    if (r->enq >= USB_TRBS - 1) {
        UsbTrb *l = &r->trb[USB_TRBS - 1];
        l->param = r->phys;
        l->status = 0;
        l->control = (6u << 10) | (1u << 1) | r->ccs;
        __sync_synchronize();
        r->enq = 0;
        r->ccs ^= 1;
    }
    UsbTrb *t = &r->trb[r->enq++];
    t->param = param;
    t->status = status;
    t->control = control | r->ccs;
    __sync_synchronize();
}

void usb_db(uint8_t slot, uint32_t target, uint32_t value)
{
    (void)value;                     /* the doorbell carries target only */
    if (!g_usb_db) return;
    W32(g_usb_db + (uint32_t)slot * 4u, target & 0xFFu);
}

void usb_enqueue(UsbRing *r, uint64_t param, uint32_t status, uint32_t control,
                 uint8_t slot, uint32_t target)
{
    uint64_t fl = spin_lock_irqsave(&g_usb_lock);
    usb_ring_push(r, param, status, control);
    usb_db(slot, target, r->enq);
    spin_unlock_irqrestore(&g_usb_lock, fl);
}

static void drain_locked(void)
{
    for (;;) {
        UsbTrb *e = &g_ev_ring.trb[g_ev_ring.enq];
        if ((e->control & 1u) != g_ev_ring.ccs) break;
        __sync_synchronize();

        uint32_t p2 = e->status;
        uint32_t p3 = e->control;
        uint32_t type = (p3 >> 10) & 0x3F;
        uint32_t code = (p2 >> 24) & 0xFF;
        uint8_t  slot = (uint8_t)((p3 >> 24) & 0xFF);
        uint8_t  ep   = (uint8_t)((p3 >> 16) & 0x1F);
        uint32_t resid = p2 & 0xFFFFFF;

        g_ev_ring.enq++;
        if (g_ev_ring.enq >= USB_TRBS) {
            g_ev_ring.enq = 0;
            g_ev_ring.ccs ^= 1;
        }

        if (type == 33) {                       /* command completion */
            g_cmd_slot = slot;
            g_cmd_code = code;
            g_cmd_done = true;
        } else if (type == 32) {                /* transfer event */
            if (ep == 1 && slot == g_ctl_slot) {
                g_ctl_code = code;
                g_ctl_done = true;
            } else if (slot == g_bulk_slot &&
                       (ep == g_bulk_ep_in || ep == g_bulk_ep_out)) {
                g_bulk_code = code;
                g_bulk_done = true;
            } else if (usb_net_event(slot, ep, code, resid)) {
                /* the network device's */
            } else {
                for (uint32_t i = 0; i < USB_MAX_DEVS; i++) {
                    UsbDev *d = &g_usb_devs[i];
                    if (d->used && d->hid_kind && d->slot == slot)
                        usb_hid_event(d, code, resid);
                }
            }
        }
        /* type 34: port status change, already handled by the reset path */
    }

    uint64_t erdp = g_ev_ring.phys + (uint64_t)g_ev_ring.enq * 16u;
    W32(RT + 0x38, (uint32_t)erdp | 8u);
    W32(RT + 0x3C, (uint32_t)(erdp >> 32));
}

void usb_drain(void)
{
    uint64_t fl = spin_lock_irqsave(&g_usb_lock);
    drain_locked();
    spin_unlock_irqrestore(&g_usb_lock, fl);
}

static void port_scan(void);

void usb_poll_thread(void *arg)
{
    (void)arg;
    for (uint32_t n = 0;; n++) {
        usb_drain();
        if (n % 64 == 0) port_scan();           /* ~8 times a second: plugged in, pulled out (a phone
                                                   switching mode gives up after a second or two) */
        sched_sleep_ms(2);
    }
}

/* a command TRB: `control` is its third dword (type, slot, endpoint...) */
static uint32_t usb_cmd_raw(uint32_t control, uint64_t ptr, uint32_t *out_slot)
{
    uint64_t fl = spin_lock_irqsave(&g_usb_lock);
    g_cmd_done = false;
    usb_ring_push(&g_cmd_ring, ptr, 0, control);
    usb_db(0, 0, g_cmd_ring.enq);
    spin_unlock_irqrestore(&g_usb_lock, fl);

    extern uint64_t sched_clock_ns(void);
    uint64_t start = usb_tsc(), relaxed = sched_clock_ns();
    while (!g_cmd_done) {
        usb_drain();
        if (usb_tsc() - start > 10000000000ULL)
            return 0xFFFFFFFFu;
        usb_spin();
        usb_relax(&relaxed);
    }
    if (out_slot) *out_slot = g_cmd_slot;
    return g_cmd_code;
}

uint32_t usb_cmd(uint32_t type, uint32_t slot, uint64_t ptr, uint32_t *out_slot)
{
    return usb_cmd_raw((type << 10) | (slot << 24), ptr, out_slot);
}

/* An endpoint that halted (a STALL, a transaction error) or timed out,
   usable again: Reset Endpoint (halted) or Stop Endpoint (still running,
   a transfer pending), its ring's dequeue moved past everything queued,
   and the device's own halt cleared (CLEAR_FEATURE ENDPOINT_HALT). */
static void usb_recover_ep(UsbDev *d, uint8_t ep_id, UsbRing *ring, bool halted)
{
    uint32_t ep = ((uint32_t)ep_id << 16) | ((uint32_t)d->slot << 24);
    usb_cmd_raw((halted ? 14u : 15u) << 10 | ep, 0, NULL);

    uint64_t fl = spin_lock_irqsave(&g_usb_lock);
    if (ring->enq >= USB_TRBS - 1) {          /* at the link TRB: the next lap */
        ring->enq = 0;
        ring->ccs ^= 1;
    }
    uint64_t deq = (ring->phys + (uint64_t)ring->enq * 16u) | ring->ccs;
    spin_unlock_irqrestore(&g_usb_lock, fl);
    usb_cmd_raw((16u << 10) | ep, deq, NULL);

    uint16_t addr = (uint16_t)((ep_id >> 1) | ((ep_id & 1u) ? 0x80u : 0u));
    if (ep_id != 1)                            /* ep0: a STALL is the answer, not a halt */
        usb_control(d, 0x02, 1, 0, addr, NULL, 0, false);
    usb_log_hex(halted ? "endpoint halted, reset: ep=" : "endpoint timed out, stopped: ep=", ep_id);
}

bool usb_control(UsbDev *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
                 void *buf, uint16_t len, bool in)
{
    if (len > 512) len = 512;
    if (len && !in) memcpy(g_ctl_buf, buf, len);
    if (len && in) memset(g_ctl_buf, 0, len);   /* a short answer: the rest reads as zero */

    uint64_t fl = spin_lock_irqsave(&g_usb_lock);
    g_ctl_slot = d->slot;
    g_ctl_done = false;

    uint64_t setup = (uint64_t)(rt | (req << 8) | ((uint32_t)val << 16)) |
                     ((uint64_t)(idx | ((uint32_t)len << 16)) << 32);
    uint32_t styp = len ? (in ? (3u << 16) : (2u << 16)) : 0;
    usb_ring_push(&d->ep0, setup, 8, (2u << 10) | (1u << 6) | styp);

    if (len)
        usb_ring_push(&d->ep0, g_ctl_buf_phys, len,
                      (3u << 10) | (in ? (1u << 16) : 0));

    usb_ring_push(&d->ep0, 0, 0,
                  (4u << 10) | (1u << 5) |
                  ((len && in) ? 0 : (1u << 16)));
    usb_db(d->slot, 1, d->ep0.enq);
    spin_unlock_irqrestore(&g_usb_lock, fl);

    extern uint64_t sched_clock_ns(void);
    uint64_t start = usb_tsc(), relaxed = sched_clock_ns();
    while (!g_ctl_done) {
        usb_drain();
        if (usb_tsc() - start > 10000000000ULL) return false;
        usb_spin();
        usb_relax(&relaxed);
    }
    if (g_ctl_code == 6) {                     /* a STALL halts ep0 too: usable again */
        usb_recover_ep(d, 1, &d->ep0, true);
        return false;
    }
    if (g_ctl_code != 1 && g_ctl_code != 13) return false;
    if (len && in) memcpy(buf, g_ctl_buf, len);
    return true;
}

bool usb_get_desc(UsbDev *d, uint8_t type, uint8_t index, void *buf, uint16_t len)
{
    return usb_control(d, 0x80, 6, (uint16_t)((type << 8) | index), 0, buf, len, true);
}

bool usb_set_config(UsbDev *d, uint8_t value)
{
    return usb_control(d, 0x00, 9, value, 0, NULL, 0, false);
}

bool usb_set_protocol(UsbDev *d, uint8_t iface, uint8_t proto)
{
    (void)proto;
    return usb_control(d, 0x21, 0x0B, 0, iface, NULL, 0, false) &&
           usb_control(d, 0x21, 0x0A, 0, iface, NULL, 0, false);
}

static bool address_device(UsbDev *d)
{
    memset(d->ctx, 0, PAGE_SIZE);
    memset(d->in, 0, PAGE_SIZE);

    uint32_t *icc = (uint32_t *)d->in;
    icc[1] = 3u;                                /* slot + EP0 */

    uint8_t *slot = d->in + g_usb_ctx_stride;
    *(uint32_t *)(slot + 0) = ((uint32_t)d->speed << 20) | (1u << 27);
    *(uint32_t *)(slot + 4) = (uint32_t)d->port << 16;

    uint8_t *ep0 = d->in + 2u * g_usb_ctx_stride;
    *(uint32_t *)(ep0 + 0) = 0;
    *(uint32_t *)(ep0 + 4) = ((uint32_t)d->ep0_maxp << 16) | (4u << 3) | (3u << 1);
    *(uint64_t *)(ep0 + 8) = d->ep0.phys | 1u;
    *(uint32_t *)(ep0 + 16) = 8;

    g_dcbaa[d->slot] = d->ctx_phys;
    __sync_synchronize();

    return usb_cmd(11, d->slot, d->in_phys, NULL) == 1;
}

bool usb_configure_ep(UsbDev *d, uint8_t ep_id, uint8_t type, uint16_t maxp,
                      uint8_t interval, UsbRing *ring)
{
    memset(d->in, 0, PAGE_SIZE);

    uint32_t *icc = (uint32_t *)d->in;
    icc[1] = 1u | (1u << ep_id);

    uint8_t *slot = d->in + g_usb_ctx_stride;
    /* Context Entries: the highest endpoint so far (a lower one would
       drop the others) */
    if (ep_id > d->max_ep) d->max_ep = ep_id;
    *(uint32_t *)(slot + 0) = ((uint32_t)d->speed << 20) |
                              ((uint32_t)d->max_ep << 27);
    *(uint32_t *)(slot + 4) = (uint32_t)d->port << 16;

    uint8_t *ep = d->in + (uint32_t)(ep_id + 1) * g_usb_ctx_stride;
    *(uint32_t *)(ep + 0) = (uint32_t)interval << 16;
    *(uint32_t *)(ep + 4) = ((uint32_t)maxp << 16) | ((uint32_t)type << 3) | (3u << 1);
    *(uint64_t *)(ep + 8) = ring->phys | 1u;
    *(uint32_t *)(ep + 16) = maxp;

    __sync_synchronize();
    return usb_cmd(12, d->slot, d->in_phys, NULL) == 1;
}

bool usb_bulk_xfer(UsbDev *d, uint8_t ep_id, UsbRing *ring, uint64_t phys,
                   uint32_t len, bool in, uint32_t timeout_ms)
{
    /* A TRB's buffer must not cross a 64 KB boundary (xHCI 4.11.7.1): one
       Normal TRB per piece, chained (CH), the event on the last (IOC) or
       on a short packet (ISP); TD Size: the packets still to come. */
    uint32_t maxp = in ? d->st_maxp_in : d->st_maxp_out;
    if (!maxp) maxp = 512;
    uint64_t fl = spin_lock_irqsave(&g_usb_lock);
    g_bulk_slot = d->slot;
    g_bulk_ep_in  = in ? ep_id : 0xFF;
    g_bulk_ep_out = in ? 0xFF : ep_id;
    g_bulk_done = false;
    uint64_t p = phys;
    uint32_t left = len;
    do {
        uint32_t room = 0x10000u - (uint32_t)(p & 0xFFFFu);
        uint32_t n = left < room ? left : room;
        uint32_t after = left - n;
        uint32_t td = (after + maxp - 1) / maxp;
        if (td > 31) td = 31;
        bool last = after == 0;
        usb_ring_push(ring, p, n | (td << 17),
                      (1u << 10) | (1u << 2) | (last ? (1u << 5) : (1u << 4)));
        p += n;
        left = after;
    } while (left);
    usb_db(d->slot, ep_id, ring->enq);
    spin_unlock_irqrestore(&g_usb_lock, fl);

    /* the stick's answer (1-50 ms a write, 100+ while it tidies itself):
       not with the big lock held -- the stick log writes every second
       (kernel/sticklog.c), and the whole kernel waited for the stick */
    extern uint64_t sched_clock_ns(void);
    uint64_t start = usb_tsc(), relaxed = sched_clock_ns();
    uint64_t limit = (uint64_t)timeout_ms * 5000000ULL;
    while (!g_bulk_done) {
        usb_drain();
        if (usb_tsc() - start > limit) {
            usb_recover_ep(d, ep_id, ring, false);
            return false;
        }
        usb_spin();
        usb_relax(&relaxed);
    }
    if (g_bulk_code == 1 || g_bulk_code == 13) return true;
    if (g_bulk_code == 6 || g_bulk_code == 4)     /* STALL, transaction error */
        usb_recover_ep(d, ep_id, ring, true);
    return false;
}

static int reset_port(uint32_t port)
{
    volatile uint8_t *p = OP + 0x400u + (port - 1u) * 0x10u;
    uint32_t v = R32(p);

    /* Firmware may leave the port unpowered; bring it up, then give a
       just-powered device time to attach. A powered empty port is empty. */
    bool powered_now = false;
    if (!(v & PORTSC_PP)) {
        W32(p, v | PORTSC_PP);
        for (int i = 0; i < 200 && !(R32(p) & PORTSC_PP); i++)
            usb_delay_ms(1);
        v = R32(p);
        powered_now = true;
    }
    if (powered_now) {
        for (int i = 0; i < 100 && !(v & PORTSC_CCS); i++) {
            usb_delay_ms(1);
            v = R32(p);
        }
    }
    if (!(v & PORTSC_CCS)) return 0;

    W32(p, v & ~PORTSC_PED);                    /* ack stale change bits */
    usb_spin();

    v = R32(p);
    W32(p, (v & ~PORTSC_PED) | PORTSC_PR);

    uint64_t start = usb_tsc();
    while (R32(p) & PORTSC_PR) {
        if (usb_tsc() - start > 3000000000ULL) return 0;
        usb_spin();
    }
    usb_delay_ms(20);                           /* USB3 link training */

    v = R32(p);
    W32(p, v & ~PORTSC_PED);
    usb_spin();
    v = R32(p);
    if (!(v & PORTSC_CCS)) return 0;
    return (int)((v >> 10) & 0xF);
}

static UsbDev *dev_on_port(uint32_t port)
{
    for (uint32_t i = 0; i < USB_MAX_DEVS; i++)
        if (g_usb_devs[i].used && g_usb_devs[i].port == port) return &g_usb_devs[i];
    return NULL;
}

static UsbDev *dev_alloc(uint8_t slot)
{
    for (uint32_t i = 0; i < USB_MAX_DEVS; i++) {
        if (!g_usb_devs[i].used) {
            memset(&g_usb_devs[i], 0, sizeof(UsbDev));
            g_usb_devs[i].used = true;
            g_usb_devs[i].slot = slot;
            return &g_usb_devs[i];
        }
    }
    return NULL;
}

/* the configuration in g_cfg_buf has an interface we drive */
static bool cfg_known(uint32_t total)
{
    for (uint32_t off = 0; off + 9 <= total; ) {
        uint8_t blen = g_cfg_buf[off];
        if (blen < 2) break;
        if (g_cfg_buf[off + 1] == 4) {
            uint8_t c = g_cfg_buf[off + 5], sc = g_cfg_buf[off + 6], pr = g_cfg_buf[off + 7];
            if (c == 3 || c == 8 || (c == 0xE0 && sc == 1 && pr == 3) || (c == 2 && (sc == 6 || (sc == 2 && pr == 0xFF))))
                return true;
        }
        off += blen;
    }
    return false;
}

/* a device that failed half way: its slot back to the controller (there
   are only USB_MAX_SLOTS) */
static void drop_dev(UsbDev *d)
{
    usb_cmd(10, d->slot, 0, NULL);              /* Disable Slot */
    g_dcbaa[d->slot] = 0;
    d->used = false;
}

static bool probe_once(uint32_t port);

/* A device on a port. Address Device may fail the first time (a phone,
   still settling): its slot dropped, the port reset again, up to 3 tries. */
static void probe_port(uint32_t port)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        if (probe_once(port)) return;
        usb_delay_ms(150);
    }
}

/* false: worth another try */
static bool probe_once(uint32_t port)
{
    int speed = reset_port(port);
    if (!speed) return true;

    uint32_t slot = 0;
    uint32_t st = usb_cmd(9, 0, 0, &slot);
    if (st != 1 || slot == 0 || slot >= USB_MAX_SLOTS) {
        usb_log_hex("enable slot failed, status=", st);
        return true;
    }

    UsbDev *d = dev_alloc((uint8_t)slot);
    if (!d) return true;
    d->port = (uint8_t)port;
    d->speed = (uint8_t)speed;
    d->ep0_maxp = (speed == 2) ? 8 : ((speed == 4) ? 512 : 64);

    d->ctx = usb_pages(1, &d->ctx_phys);
    d->in  = usb_pages(1, &d->in_phys);
    void *ring = usb_pages(1, &d->ep0.phys);
    if (!d->ctx || !d->in || !ring) {
        drop_dev(d);
        return true;
    }
    d->ep0.trb = (UsbTrb *)ring;
    usb_ring_init(&d->ep0, d->ep0.phys);

    if (!address_device(d)) {
        usb_log_hex("address device failed, code=", g_cmd_code);
        drop_dev(d);
        return false;
    }

    /* ep0's packet size was a guess from the speed: a full-speed device
       may use 8, 16, 32 or 64. Its first 8 descriptor bytes say
       (bMaxPacketSize0; SuperSpeed: 2^n), and Evaluate Context fixes the
       endpoint context when the guess was wrong. */
    uint8_t desc[18];
    if (usb_get_desc(d, 1, 0, desc, 8)) {
        uint16_t mps = (speed >= 4) ? (uint16_t)(1u << (desc[7] & 0xF)) : desc[7];
        if (mps >= 8 && mps != d->ep0_maxp) {
            d->ep0_maxp = mps;
            uint32_t *icc = (uint32_t *)d->in;
            icc[0] = 0;
            icc[1] = 2u;                            /* EP0 only */
            uint8_t *ep0 = d->in + 2u * g_usb_ctx_stride;
            *(uint32_t *)(ep0 + 4) = ((uint32_t)mps << 16) | (4u << 3) | (3u << 1);
            __sync_synchronize();
            if (usb_cmd(13, d->slot, d->in_phys, NULL) != 1)
                usb_log_hex("evaluate context (ep0 size) failed, size=", mps);
        }
    }
    if (!usb_get_desc(d, 1, 0, desc, sizeof(desc))) {
        usb_log("device descriptor read failed");
        drop_dev(d);
        return false;
    }

    uint16_t total = 0;
    if (!usb_get_desc(d, 2, 0, g_cfg_buf, 512)) {
        usb_log("configuration descriptor read failed");
        drop_dev(d);
        return false;
    }
    total = le16(g_cfg_buf + 2);
    if (total < 9 || total > 512) total = 512;
    /* a device with more configurations (a phone, a modem: one is often
       vendor-only): the second, when only it has something we drive */
    if (desc[17] > 1 && !cfg_known(total) && usb_get_desc(d, 2, 1, g_cfg_buf, 512)) {
        uint16_t t2 = le16(g_cfg_buf + 2);
        if (t2 >= 9 && t2 <= 512 && cfg_known(t2)) total = t2;
        else if (usb_get_desc(d, 2, 0, g_cfg_buf, 512)) total = le16(g_cfg_buf + 2) > 512 ? 512 : le16(g_cfg_buf + 2);
    }

    uint8_t cfg_value = g_cfg_buf[5];
    if (!usb_set_config(d, cfg_value)) {
        usb_log("set configuration failed");
        drop_dev(d);
        return false;
    }

    /* One HID interface is driven: a device with several (a gaming mouse
       is "mouse + keyboard" in one) gives its mouse, else its keyboard,
       else its first -- each endpoint with ITS interface's number and
       protocol (they used to be the last interface's: the mouse's
       endpoint went to the keyboard's interface). */
    uint8_t iface = 0, cls = 0, sub = 0, proto = 0, alt = 0;
    UsbNetInfo ni;
    memset(&ni, 0, sizeof ni);
    uint8_t hid_ep = 0, st_in = 0, st_out = 0, hid_iface = 0, hid_proto = 0;
    uint16_t hid_maxp = 0, st_maxp_in = 0, st_maxp_out = 0;
    uint8_t hid_interval = 0;
    int hid_rank = 0;                     /* 3 mouse, 2 keyboard, 1 other HID */

    for (uint32_t off = 0; off + 2 <= total; ) {
        uint8_t blen = g_cfg_buf[off];
        uint8_t btype = g_cfg_buf[off + 1];
        if (blen < 2) break;

        if (btype == 4 && blen >= 9) {
            iface = g_cfg_buf[off + 2];
            alt = g_cfg_buf[off + 3];
            cls = g_cfg_buf[off + 5];
            sub = g_cfg_buf[off + 6];
            proto = g_cfg_buf[off + 7];
            /* the network's control interface: RNDIS (Android's tethering
               is "wireless controller"; Windows-style is CDC, vendor
               protocol), or CDC ECM */
            if (!ni.kind && ((cls == 0xE0 && sub == 1 && proto == 3) || (cls == 2 && sub == 2 && proto == 0xFF))) {
                ni.kind = 1; ni.comm_iface = iface;
            } else if (!ni.kind && cls == 2 && sub == 6) {
                ni.kind = 2; ni.comm_iface = iface;
            }
        } else if (btype == 0x24 && blen >= 4 && g_cfg_buf[off + 2] == 0x0F) {
            ni.mac_str = g_cfg_buf[off + 3];      /* CDC Ethernet: iMACAddress */
        } else if (btype == 5 && blen >= 7) {
            uint8_t addr = g_cfg_buf[off + 2];
            uint8_t attr = g_cfg_buf[off + 3] & 0x3;
            uint16_t maxp = le16(g_cfg_buf + off + 4);
            uint8_t interval = g_cfg_buf[off + 6];

            if (cls == 3 && attr == 3 && (addr & 0x80)) {
                int rank = proto == 2 ? 3 : proto == 1 ? 2 : 1;
                if (rank > hid_rank) {
                    hid_rank = rank;
                    hid_ep = addr;
                    hid_maxp = maxp;
                    hid_interval = interval;
                    hid_iface = iface;
                    hid_proto = proto;
                }
            } else if (cls == 0x0A && attr == 2 && ni.kind) {
                ni.data_iface = iface;
                ni.data_alt = alt;
                if (addr & 0x80) { ni.in_addr = addr; ni.in_maxp = maxp; }
                else { ni.out_addr = addr; ni.out_maxp = maxp; }
            } else if (cls == 8 && attr == 2) {
                if (addr & 0x80) {
                    st_in = addr;
                    st_maxp_in = maxp;
                } else {
                    st_out = addr;
                    st_maxp_out = maxp;
                }
            }
        }
        off += blen;
    }

    usb_log_hex("port device, speed=", (uint64_t)speed);
    if (!hid_ep && !st_in && !ni.in_addr) {
        usb_log_hex("no HID/storage interface; config bytes=", total);
        usb_log_hex("  first interface class=", g_cfg_buf[9 + 5]);
        usb_log_hex("  device class=", desc[4]);
    }

    if (hid_ep && hid_maxp) {
        usb_log_hex("HID interface=", hid_iface);
        usb_log_hex("HID protocol=", hid_proto);
        usb_log_hex("HID maxp=", hid_maxp);
        if (usb_hid_setup(d, hid_iface, hid_proto, hid_ep, hid_maxp, hid_interval))
            usb_log(hid_proto == 2 ? "HID mouse ready" : "HID keyboard ready");
        else
            usb_log("HID setup failed");
    }

    if (ni.kind && ni.in_addr && ni.out_addr && ni.in_maxp) {
        if (!usb_net_setup(d, &ni)) usb_log("network device setup failed");
    }

    if (st_in && st_out && st_maxp_in) {
        if (usb_storage_setup(d, st_in, st_out, st_maxp_in, st_maxp_out))
            usb_log("mass storage ready");
        else
            usb_log("mass storage setup failed");
    }
    return true;
}

/* Hotplug: a port whose connection changed (CSC) gets a device probed on
   it, or the device that was there let go (Disable Slot). The boot stick
   in use is never let go: the system may be reading it. */
static void port_scan(void)
{
    for (uint32_t port = 1; port <= g_usb_max_ports && port <= USB_MAX_PORTS; port++) {
        volatile uint8_t *p = OP + 0x400u + (port - 1u) * 0x10u;
        uint32_t v = R32(p);
        if (!(v & PORTSC_CSC)) continue;
        UsbDev *d = dev_on_port(port);
        /* what was there is gone -- or was replaced between two looks: a
           phone switching USB mode (file transfer -> tethering) drops off
           and comes back as another device within a few ms, and taking
           it for the old one left it never configured (the phone then
           falls back to "charging only") */
        /* still enabled: the device never left (a connect-change with no
           disconnect, seen on real machines): nothing to do. Disabled:
           it went away, or went and came back. */
        if (d && (v & PORTSC_CCS) && (v & PORTSC_PED)) {
            W32(p, (v & ~(PORTSC_PED | PORTSC_CHG)) | PORTSC_CSC);
            continue;
        }
        W32(p, (v & ~(PORTSC_PED | PORTSC_CHG)) | PORTSC_CSC);
        if (d && d != g_usb_storage) {
            usb_log_hex((v & PORTSC_CCS) ? "device replaced, port=" : "device pulled out, port=", port);
            usb_net_gone(d);
            usb_cmd(10, d->slot, 0, NULL);      /* Disable Slot */
            g_dcbaa[d->slot] = 0;
            d->used = false;
            d = NULL;
        }
        if ((v & PORTSC_CCS) && !d) {
            usb_log_hex("device plugged in, port=", port);
            /* USB 2.0 7.1.7.3: 100 ms for the contacts to settle before
               the reset -- resetting at once failed a phone's addressing */
            sched_sleep_ms(150);
            if (!(R32(p) & PORTSC_CCS)) continue;
            probe_port(port);
        }
    }
}

/* xHCI extended capability 1: firmware may still own the controller for
   pre-boot USB keyboard/mouse emulation (UEFI uses this too). */
static void usb_legacy_handoff(uint32_t hcc1)
{
    uint32_t off = (hcc1 >> 16) & 0xFFFFu;

    for (int guard = 0; off && guard < 64; guard++) {
        volatile uint32_t *cap = (volatile uint32_t *)(g_mmio + off * 4u);
        uint32_t v = *cap;
        uint32_t id = v & 0xFFu;
        uint32_t next = (v >> 8) & 0xFFu;

        if (id == 1) {
            uint32_t s = *cap;
            *cap = s | (1u << 24);              /* HC OS Owned */
            bool ok = false;
            for (int i = 0; i < 1000; i++) {
                if (!(*cap & (1u << 16))) {     /* BIOS Owned clear */
                    ok = true;
                    break;
                }
                usb_delay_ms(1);
            }
            cap[1] = 0;                         /* disable legacy SMIs */
            usb_log(ok ? "legacy handoff OK" : "legacy handoff timeout");
            return;
        }
        if (!next) break;
        off += next;
    }
}

void usb_init(void)
{
    PciDevice dev;
    if (!pci_find_device(0x0C, 0x03, &dev)) {
        usb_log("no xHCI controller");
        return;
    }
    if (dev.prog_if != 0x30) {
        usb_log("USB controller is not xHCI");
        return;
    }

    uint32_t bar0 = pci_read_config(dev.bus, dev.slot, dev.func, 0x10);
    uint64_t mmio = bar0 & ~0xFULL;
    if ((bar0 & 0x6u) == 0x4u) {
        uint32_t bar1 = pci_read_config(dev.bus, dev.slot, dev.func, 0x14);
        mmio |= (uint64_t)bar1 << 32;
    }
    if (!mmio) {
        usb_log("xHCI BAR0 is not a memory BAR");
        return;
    }

    uint32_t pcicmd = pci_read_config(dev.bus, dev.slot, dev.func, 0x04);
    pci_write_config(dev.bus, dev.slot, dev.func, 0x04,
                     pcicmd | 0x6u);            /* memory + bus master */

    g_mmio = (volatile uint8_t *)vmm_map_mmio(mmio, 0x10000);
    if (!g_mmio) {
        usb_log("failed to map xHCI registers");
        return;
    }

    uint32_t caplen = *(volatile uint8_t *)g_mmio;
    uint32_t hcs1 = R32(g_mmio + 0x04);
    uint32_t hcc1 = R32(g_mmio + 0x10);
    uint32_t dboff = R32(g_mmio + 0x14);
    uint32_t rtsoff = R32(g_mmio + 0x18);

    g_usb_op = g_mmio + caplen;
    g_usb_rt = g_mmio + rtsoff;
    g_usb_db = g_mmio + dboff;
    g_usb_ctx_stride = (hcc1 & 4u) ? 64u : 32u;
    g_usb_ac64 = (hcc1 & 1u) != 0;
    usb_legacy_handoff(hcc1);
    g_usb_max_ports = (hcs1 >> 24) & 0xFFu;
    uint32_t max_slots = hcs1 & 0xFFu;
    if (max_slots > USB_MAX_SLOTS) max_slots = USB_MAX_SLOTS;
    if (max_slots == 0) max_slots = 1;

    usb_log_hex("xHCI ports=", g_usb_max_ports);
    usb_log_hex("xHCI max slots=", max_slots);

    spin_init(&g_usb_lock, "usb");

    uint64_t cmd_phys = 0, ev_phys = 0, erst_phys = 0, dcbaa_phys = 0;
    void *cmd = usb_pages(1, &cmd_phys);
    void *ev  = usb_pages(1, &ev_phys);
    void *er  = usb_pages(1, &erst_phys);
    void *dc  = usb_pages(1, &dcbaa_phys);
    g_cfg_buf = usb_pages(1, &g_cfg_buf_phys);
    g_ctl_buf = usb_pages(1, &g_ctl_buf_phys);
    if (!cmd || !ev || !er || !dc || !g_cfg_buf || !g_ctl_buf) {
        usb_log("out of memory for xHCI structures");
        return;
    }

    g_cmd_ring.trb = (UsbTrb *)cmd;
    usb_ring_init(&g_cmd_ring, cmd_phys);
    g_ev_ring.trb = (UsbTrb *)ev;
    g_ev_ring.phys = ev_phys;
    g_ev_ring.enq = 0;
    g_ev_ring.ccs = 1;
    g_dcbaa = (uint64_t *)dc;
    g_erst_phys = erst_phys;

    /* Scratchpad: pages the controller keeps its own state in, handed over
       through DCBAA[0]. QEMU asks for none; a real Intel xHCI won't address
       a device without them. */
    uint32_t hcs2 = R32(g_mmio + 0x08);
    uint32_t nsp = (((hcs2 >> 21) & 0x1Fu) << 5) | ((hcs2 >> 27) & 0x1Fu);
    usb_log_hex("xHCI scratchpad pages=", nsp);
    if (nsp) {
        uint64_t arr_phys = 0;
        uint64_t *arr = usb_pages(1, &arr_phys);
        if (!arr || nsp > 512) {
            usb_log("out of memory for the xHCI scratchpad");
            return;
        }
        for (uint32_t i = 0; i < nsp; i++) {
            uint64_t page = 0;
            if (!usb_pages(1, &page)) {
                usb_log("out of memory for the xHCI scratchpad");
                return;
            }
            arr[i] = page;
        }
        g_dcbaa[0] = arr_phys;
    }

    /* Event Ring Segment Table: one segment of USB_TRBS TRBs. */
    *(uint64_t *)((uint8_t *)er + 0) = ev_phys;
    *(uint32_t *)((uint8_t *)er + 8) = USB_TRBS;

    uint64_t start = usb_tsc();
    while (R32(OP + 0x04) & USBSTS_CNR) {
        if (usb_tsc() - start > 3000000000ULL) {
            usb_log("controller not ready");
            return;
        }
        usb_spin();
    }

    W32(OP + 0x00, USBCMD_HCRST);
    start = usb_tsc();
    while (R32(OP + 0x00) & USBCMD_HCRST) {
        if (usb_tsc() - start > 3000000000ULL) {
            usb_log("controller reset timed out");
            return;
        }
        usb_spin();
    }

    W32(OP + 0x08, 1);                          /* 4 KB page size */
    W32(OP + 0x30, (uint32_t)dcbaa_phys);
    W32(OP + 0x34, (uint32_t)(dcbaa_phys >> 32));
    W32(OP + 0x38, max_slots);
    W32(OP + 0x18, (uint32_t)(cmd_phys | 1u));
    W32(OP + 0x1C, (uint32_t)(cmd_phys >> 32));

    W32(RT + 0x20, 0);                          /* IMAN: no interrupts */
    W32(RT + 0x24, 0);                          /* IMOD */
    W32(RT + 0x28, 1);                          /* ERSTSZ */
    W32(RT + 0x30, (uint32_t)erst_phys);
    W32(RT + 0x34, (uint32_t)(erst_phys >> 32));
    W32(RT + 0x38, (uint32_t)ev_phys | 8u);     /* ERDP, EHB */
    W32(RT + 0x3C, (uint32_t)(ev_phys >> 32));

    __sync_synchronize();
    W32(OP + 0x00, USBCMD_RUN);

    start = usb_tsc();
    while ((R32(OP + 0x04) & USBSTS_HCH) && usb_tsc() - start < 3000000000ULL)
        usb_spin();

    if (R32(OP + 0x04) & USBSTS_HCH) {
        usb_log("controller did not start");
        return;
    }

    /* After the reset every device re-attaches on its own time: USB 2
       debounces for 100 ms, USB 3 trains its link. QEMU's are there at
       once; a real machine's are not -- probing right away found none. */
    usb_delay_ms(300);
    for (uint32_t port = 1; port <= g_usb_max_ports && port <= USB_MAX_PORTS; port++) {
        uint32_t v = R32(OP + 0x400u + (port - 1u) * 0x10u);
        if (v & PORTSC_CCS) usb_log_hex("connected: port=", port);
    }
    for (uint32_t port = 1; port <= g_usb_max_ports && port <= USB_MAX_PORTS; port++)
        probe_port(port);

    g_up = true;
    usb_log("xHCI ready (polled)");
    thread_create(usb_poll_thread, NULL);
}

bool usb_ready(void)
{
    return g_up;
}
