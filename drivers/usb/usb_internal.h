/*
 * WynlandOS - xHCI internals shared by the USB driver files
 */
#pragma once

#include <wynland/types.h>
#include <wynland/usb.h>
#include <wynland/spinlock.h>

#define USB_TRBS        256u
#define USB_MAX_SLOTS   8u
#define USB_MAX_DEVS    8u
#define USB_MAX_PORTS   16u

typedef struct {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} UsbTrb;

typedef struct {
    UsbTrb  *trb;
    uint64_t phys;
    uint32_t enq;
    uint8_t  ccs;
} UsbRing;

typedef struct UsbDev {
    bool     used;
    uint8_t  slot, port, speed;
    uint8_t  max_ep;                         /* the highest endpoint id configured */
    uint8_t  ep0_maxp;

    UsbRing  ep0;
    uint8_t *ctx;       uint64_t ctx_phys;   /* device context       */
    uint8_t *in;        uint64_t in_phys;    /* input context        */

    /* HID */
    int      hid_kind;                       /* 1 keyboard, 2 mouse   */
    uint8_t  hid_ep;                         /* endpoint id           */
    uint16_t hid_maxp;
    UsbRing  hid_ring;
    uint8_t *hid_buf;   uint64_t hid_buf_phys;
    uint8_t  hid_prev[8];
    uint8_t  hid_buttons;
    bool     hid_started;

    /* Mass storage (BOT) */
    bool     storage;
    uint8_t  st_in, st_out;                  /* endpoint ids          */
    uint16_t st_maxp_in, st_maxp_out;
    UsbRing  st_ring_in, st_ring_out;
} UsbDev;

extern UsbDev  g_usb_devs[USB_MAX_DEVS];
extern Spinlock g_usb_lock;
extern UsbDev *g_usb_storage;
extern uint32_t g_usb_max_ports;

extern volatile uint8_t *g_usb_op;
extern volatile uint8_t *g_usb_rt;
extern volatile uint8_t *g_usb_db;
extern uint32_t g_usb_ctx_stride;            /* 32 or 64 bytes        */

/* ---- xhci.c ------------------------------------------------------- */
void *usb_pages(uint32_t count, uint64_t *phys);
void usb_log(const char *s);
void usb_log_hex(const char *s, uint64_t v);
void usb_poll_thread(void *arg);
void usb_drain(void);

void usb_ring_init(UsbRing *r, uint64_t phys);
void usb_ring_push(UsbRing *r, uint64_t param, uint32_t status, uint32_t control);
void usb_db(uint8_t slot, uint32_t target, uint32_t value);
void usb_enqueue(UsbRing *r, uint64_t param, uint32_t status, uint32_t control,
                 uint8_t slot, uint32_t target);

uint32_t usb_cmd(uint32_t type, uint32_t slot, uint64_t ptr, uint32_t *out_slot);
bool usb_control(UsbDev *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
                 void *buf, uint16_t len, bool in);
bool usb_get_desc(UsbDev *d, uint8_t type, uint8_t index, void *buf, uint16_t len);
bool usb_set_config(UsbDev *d, uint8_t value);
bool usb_set_protocol(UsbDev *d, uint8_t iface, uint8_t proto);
bool usb_configure_ep(UsbDev *d, uint8_t ep_id, uint8_t type, uint16_t maxp,
                      uint8_t interval, UsbRing *ring);
bool usb_bulk_xfer(UsbDev *d, uint8_t ep_id, UsbRing *ring, uint64_t phys,
                   uint32_t len, bool in, uint32_t timeout_ms);

/* ---- usb_hid.c ---------------------------------------------------- */
bool usb_hid_setup(UsbDev *d, uint8_t iface, uint8_t proto, uint8_t ep_addr,
                   uint16_t maxp, uint8_t interval);
void usb_hid_event(UsbDev *d, uint32_t code, uint32_t resid);

/* ---- usb_net.c ---------------------------------------------------- */
typedef struct {
    int      kind;                           /* 1 RNDIS, 2 CDC ECM */
    uint8_t  comm_iface, data_iface, data_alt, mac_str;
    uint8_t  in_addr, out_addr;
    uint16_t in_maxp, out_maxp;
} UsbNetInfo;
bool usb_net_setup(UsbDev *d, const UsbNetInfo *ni);
bool usb_net_event(uint8_t slot, uint8_t ep, uint32_t code, uint32_t resid);
void usb_net_gone(UsbDev *d);

/* ---- usb_storage.c ------------------------------------------------ */
bool usb_storage_setup(UsbDev *d, uint8_t ep_in, uint8_t ep_out,
                       uint16_t maxp_in, uint16_t maxp_out);
