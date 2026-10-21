/*
 * WynlandOS - USB HID (boot keyboard / mouse) over xHCI
 */

#include "usb_internal.h"
#include <wynland/irq.h>
#include <wynland/mouse.h>

static const uint16_t hid_map[256] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20,
    [0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23,
    /* i..z: HID usages are alphabetical, set-1 codes follow the rows */
    [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,   /* i j k l */
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19,   /* m n o p */
    [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14,   /* q r s t */
    [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,   /* u v w x */
    [0x1C] = 0x15, [0x1D] = 0x2C, [0x1E] = 0x02, [0x1F] = 0x03,   /* y z 1 2 */
    [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06, [0x23] = 0x07,
    [0x24] = 0x08, [0x25] = 0x09, [0x26] = 0x0A, [0x27] = 0x0B,
    [0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F,
    [0x2C] = 0x39, [0x2D] = 0x0C, [0x2E] = 0x0D, [0x2F] = 0x1A,
    [0x30] = 0x1B, [0x31] = 0x2B, [0x32] = 0x2B, [0x33] = 0x27,   /* ] \ (non-US #) ; */
    [0x34] = 0x28, [0x35] = 0x29, [0x36] = 0x33, [0x37] = 0x34,
    [0x38] = 0x35, [0x39] = 0x3A,
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E,
    [0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42,
    [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,
    [0x49] = 0x100 | 0x52, [0x4A] = 0x100 | 0x47, [0x4B] = 0x100 | 0x49,
    [0x4C] = 0x100 | 0x53, [0x4D] = 0x100 | 0x4F, [0x4E] = 0x100 | 0x51,
    [0x4F] = 0x100 | 0x4D, [0x50] = 0x100 | 0x4B, [0x51] = 0x100 | 0x50,
    [0x52] = 0x100 | 0x48, [0x53] = 0x45, [0x54] = 0x100 | 0x35,
    [0x55] = 0x37, [0x56] = 0x4A, [0x57] = 0x4E, [0x58] = 0x100 | 0x1C,
    [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51, [0x5C] = 0x4B,
    [0x5D] = 0x4C, [0x5E] = 0x4D, [0x5F] = 0x47, [0x60] = 0x48,
    [0x61] = 0x49, [0x62] = 0x52, [0x63] = 0x53,
    [0x64] = 0x56, [0x65] = 0x100 | 0x5D,                          /* ISO \, Menu */
    [0xE0] = 0x1D, [0xE1] = 0x2A, [0xE2] = 0x38,
    [0xE3] = 0x100 | 0x5B, [0xE4] = 0x100 | 0x1D, [0xE5] = 0x36,
    [0xE6] = 0x100 | 0x38, [0xE7] = 0x100 | 0x5C,
};

static void key_emit(uint8_t usage, bool down)
{
    uint16_t m = hid_map[usage];
    if (!m) return;
    if (m & 0x100) keyboard_inject_scancode(0xE0);
    uint8_t code = (uint8_t)(m & 0xFF);
    keyboard_inject_scancode(down ? code : (uint8_t)(code | 0x80));
}

static bool key_in(const uint8_t *rep, uint8_t usage)
{
    for (int i = 2; i < 8; i++)
        if (rep[i] == usage) return true;
    return false;
}

static void keyboard_report(UsbDev *d, const uint8_t *rep)
{
    for (int bit = 0; bit < 8; bit++) {
        bool old = (d->hid_prev[0] >> bit) & 1;
        bool now = (rep[0] >> bit) & 1;
        if (now && !old) key_emit((uint8_t)(0xE0 + bit), true);
        if (!now && old) key_emit((uint8_t)(0xE0 + bit), false);
    }

    for (int i = 2; i < 8; i++) {
        uint8_t k = d->hid_prev[i];
        if (k && !key_in(rep, k)) key_emit(k, false);
    }
    for (int i = 2; i < 8; i++) {
        uint8_t k = rep[i];
        if (k && !key_in(d->hid_prev, k)) key_emit(k, true);
    }

    for (int i = 0; i < 8; i++) d->hid_prev[i] = rep[i];
}

static void mouse_report(UsbDev *d, const uint8_t *rep, uint32_t len)
{
    if (len < 3) return;
    int dx = (int8_t)rep[1];
    int dy = (int8_t)rep[2];
    int wheel = (len >= 4) ? (int8_t)rep[3] : 0;
    mouse_inject_usb(dx, dy, rep[0] & 0x07, wheel);
    d->hid_buttons = rep[0] & 0x07;
}

static void hid_resubmit(UsbDev *d)
{
    usb_ring_push(&d->hid_ring, d->hid_buf_phys, d->hid_maxp,
                  (1u << 10) | (1u << 5));
    usb_db(d->slot, d->hid_ep, d->hid_ring.enq);
}

void usb_hid_event(UsbDev *d, uint32_t code, uint32_t resid)
{
    if (!d->hid_started) return;

    uint32_t len = d->hid_maxp;
    if (resid <= len) len -= resid;
    else len = 0;

    if ((code == 1 || code == 13) && len) {
        if (d->hid_kind == 1) keyboard_report(d, d->hid_buf);
        else if (d->hid_kind == 2) mouse_report(d, d->hid_buf, len);
    }

    hid_resubmit(d);
}

bool usb_hid_setup(UsbDev *d, uint8_t iface, uint8_t proto, uint8_t ep_addr,
                   uint16_t maxp, uint8_t interval)
{
    if (!maxp || maxp > 64) maxp = 8;
    d->hid_kind = (proto == 2) ? 2 : 1;
    d->hid_ep = (uint8_t)(((ep_addr & 0x0F) << 1) | (ep_addr >> 7));
    d->hid_maxp = maxp;

    void *buf = usb_pages(1, &d->hid_buf_phys);
    void *ring = usb_pages(1, &d->hid_ring.phys);
    if (!buf || !ring) return false;
    d->hid_buf = (uint8_t *)buf;
    d->hid_ring.trb = (UsbTrb *)ring;
    usb_ring_init(&d->hid_ring, d->hid_ring.phys);

    /* The endpoint context's Interval is an exponent: every 2^Interval
       x 125 us (xHCI 6.2.3.6). High/SuperSpeed descriptors already give
       one (bInterval - 1); full/low speed give milliseconds. */
    uint8_t xhci_interval;
    if (d->speed == 3 || d->speed == 4) {
        xhci_interval = interval ? (uint8_t)(interval - 1) : 0;
        if (xhci_interval > 15) xhci_interval = 15;
    } else {
        uint32_t ms = interval ? interval : 1, e = 0;
        while ((2u << e) <= ms) e++;           /* floor(log2(ms)) */
        xhci_interval = (uint8_t)(e + 3);       /* 8 x 125 us per ms */
        if (xhci_interval > 10) xhci_interval = 10;
    }

    if (!usb_configure_ep(d, d->hid_ep, 7, maxp, xhci_interval, &d->hid_ring))
        return false;

    if (proto == 1 || proto == 2)
        usb_set_protocol(d, iface, 0);

    memset(d->hid_prev, 0, sizeof(d->hid_prev));
    d->hid_started = true;

    /* Submit the first IN transfer; the poll thread drains the report. */
    usb_enqueue(&d->hid_ring, d->hid_buf_phys, maxp,
                (1u << 10) | (1u << 5), d->slot, d->hid_ep);
    return true;
}
