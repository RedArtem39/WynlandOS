/*
 * WynlandOS - PS/2 Mouse Driver Implementation
 */

#include <wynland/memlayout.h>
#include <wynland/mouse.h>

#define CURSOR_W 18
#define CURSOR_H 19

static const uint8_t cursor_mask[19][18] = {
    { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 1, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 1, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 1, 0, 0, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 0, 0, 0, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

static BootInfo *g_boot_info = NULL;
static int32_t mouse_x = 0;
static int32_t mouse_y = 0;
static int32_t bound_w = 1, bound_h = 1;   /* mouse_set_bounds() */
static uint8_t mouse_buttons = 0;
static bool mouse_has_wheel = false;   /* IntelliMouse: 4-byte packets */
static int8_t mouse_wheel = 0;         /* this packet's wheel delta */


static uint32_t saved_background[CURSOR_W * CURSOR_H];
static bool cursor_visible = false;

extern void serial_write_string(const char *str);

static inline uint8_t inb(uint16_t port)
{
    uint8_t data;
    __asm__ volatile("inb %1, %0" : "=a"(data) : "Nd"(port));
    return data;
}

static inline void outb(uint16_t port, uint8_t data)
{
    __asm__ volatile("outb %0, %1" :: "a"(data), "Nd"(port));
}

static inline void io_wait(void)
{
    outb(0x80, 0);
}

static void mouse_wait_write(void)
{
    int timeout = 100000;
    while (timeout > 0) {
        if ((inb(0x64) & 2) == 0) {
            return;
        }
        timeout--;
    }
}

static void mouse_wait_read(void)
{
    int timeout = 100000;
    while (timeout > 0) {
        if ((inb(0x64) & 1) == 1) {
            return;
        }
        timeout--;
    }
}

static void mouse_write_cmd(uint8_t cmd)
{
    mouse_wait_write();
    outb(0x64, cmd);
}

static void mouse_write_data(uint8_t data)
{
    mouse_wait_write();
    outb(0x60, data);
}

static uint8_t mouse_read(void)
{
    mouse_wait_read();
    return inb(0x60);
}

static void mouse_write_mouse(uint8_t val)
{
    mouse_write_cmd(0xD4);
    mouse_write_data(val);
}

static void mouse_save_background(void)
{
    uint32_t *fb = (uint32_t *)P2V(g_boot_info->fb_addr);
    uint32_t pitch_pixels = g_boot_info->fb_pitch / 4;

    for (int y = 0; y < CURSOR_H; y++) {
        int screen_y = mouse_y + y;
        if (screen_y >= (int)g_boot_info->fb_height) break;
        for (int x = 0; x < CURSOR_W; x++) {
            int screen_x = mouse_x + x;
            if (screen_x >= (int)g_boot_info->fb_width) break;
            saved_background[y * CURSOR_W + x] = fb[screen_y * pitch_pixels + screen_x];
        }
    }
}

static void mouse_restore_background(void)
{
    uint32_t *fb = (uint32_t *)P2V(g_boot_info->fb_addr);
    uint32_t pitch_pixels = g_boot_info->fb_pitch / 4;

    for (int y = 0; y < CURSOR_H; y++) {
        int screen_y = mouse_y + y;
        if (screen_y >= (int)g_boot_info->fb_height) break;
        for (int x = 0; x < CURSOR_W; x++) {
            int screen_x = mouse_x + x;
            if (screen_x >= (int)g_boot_info->fb_width) break;
            fb[screen_y * pitch_pixels + screen_x] = saved_background[y * CURSOR_W + x];
        }
    }
}

static void mouse_draw_cursor(void)
{
    uint32_t *fb = (uint32_t *)P2V(g_boot_info->fb_addr);
    uint32_t pitch_pixels = g_boot_info->fb_pitch / 4;

    for (int y = 0; y < CURSOR_H; y++) {
        int screen_y = mouse_y + y;
        if (screen_y >= (int)g_boot_info->fb_height) break;
        for (int x = 0; x < CURSOR_W; x++) {
            int screen_x = mouse_x + x;
            if (screen_x >= (int)g_boot_info->fb_width) break;

            uint8_t pixel_type = cursor_mask[y][x];
            if (pixel_type == 1) {
                fb[screen_y * pitch_pixels + screen_x] = 0x00000000; /* Black outline */
            } else if (pixel_type == 2) {
                if (mouse_buttons & 1) {
                    fb[screen_y * pitch_pixels + screen_x] = 0x00FF3333; /* Red on Left Click */
                } else if (mouse_buttons & 2) {
                    fb[screen_y * pitch_pixels + screen_x] = 0x0033FF33; /* Green on Right Click */
                } else {
                    fb[screen_y * pitch_pixels + screen_x] = 0x00FFFFFF; /* White */
                }
            }
        }
    }
}

static int mouse_hide_count = 0;

/* With virtio-gpu up, the pointer lives on the GPU's cursor plane (see
   drivers/video/virtio_gpu.c): the software cursor below would only
   scribble into the GOP framebuffer, which isn't even being scanned out.
   Nor when a program holds the framebuffer (g_quiet_console: wynvt) --
   its pixels are its own, the kernel neither draws nor "restores" them. */
static bool mouse_hw_cursor(void)
{
    extern bool virtio_gpu_is_active(void);
    extern bool g_quiet_console;
    return virtio_gpu_is_active() || g_quiet_console;
}

void mouse_hide(void)
{
    if (mouse_hw_cursor()) return;

    if (mouse_hide_count == 0 && cursor_visible) {
        mouse_restore_background();
        cursor_visible = false;
    }
    mouse_hide_count++;
}

void mouse_show(void)
{
    if (mouse_hw_cursor()) return;

    mouse_hide_count--;
    if (mouse_hide_count <= 0) {
        mouse_hide_count = 0;
        if (!cursor_visible && g_boot_info) {
            mouse_save_background();
            mouse_draw_cursor();
            cursor_visible = true;
        }
    }
}

void mouse_init(BootInfo *info)
{
    g_boot_info = info;
    mouse_x = info->fb_width / 2;
    mouse_y = info->fb_height / 2;
    bound_w = (int32_t)info->fb_width;
    bound_h = (int32_t)info->fb_height;
    mouse_buttons = 0;
    cursor_visible = false;

    serial_write_string("Mouse: Initializing PS/2 Mouse...\r\n");

    /* 1. Enable Auxiliary Device */
    mouse_write_cmd(0xA8);

    /* 2. Enable interrupts */
    mouse_write_cmd(0x20);
    uint8_t status = mouse_read();
    status |= 0x02;  /* Enable Auxiliary Interrupt (IRQ12) */
    status &= ~0x20; /* Enable Auxiliary Clock */
    mouse_write_cmd(0x60);
    mouse_write_data(status);

    /* 3. Set default settings */
    mouse_write_mouse(0xF6);
    mouse_read(); /* Read ACK */

    /* 3b. IntelliMouse knock (sample rate 200, 100, 80): a wheel mouse
       then reports ID 3 and sends a 4th byte, the wheel delta. */
    static const uint8_t knock[3] = { 200, 100, 80 };
    for (int i = 0; i < 3; i++) {
        mouse_write_mouse(0xF3); mouse_read();
        mouse_write_mouse(knock[i]); mouse_read();
    }
    mouse_write_mouse(0xF2); mouse_read();   /* get ID: ACK, then the ID */
    mouse_has_wheel = mouse_read() == 3;
    serial_write_string(mouse_has_wheel ? "Mouse: wheel (IntelliMouse)\r\n" : "Mouse: no wheel\r\n");

    /* 4. Enable data reporting */
    mouse_write_mouse(0xF4);
    mouse_read(); /* Read ACK */

    /* 5. Enable IRQ12 on Slave PIC and IRQ2 on Master PIC */
    uint8_t mask = inb(0xA1);
    mask &= ~0x10; /* Unmask IRQ12 */
    outb(0xA1, mask);

    /* Save background and render cursor */
    mouse_save_background();
    mouse_draw_cursor();
    cursor_visible = true;

    serial_write_string("Mouse: PS/2 Mouse initialized successfully.\r\n");
}

#define MOUSE_QUEUE_SIZE 1024
static uint8_t mouse_queue[MOUSE_QUEUE_SIZE];
static uint32_t mouse_queue_head = 0;
static uint32_t mouse_queue_tail = 0;

static void mouse_queue_push(uint8_t val)
{
    uint32_t next = (mouse_queue_head + 1) % MOUSE_QUEUE_SIZE;
    if (next != mouse_queue_tail) {
        mouse_queue[mouse_queue_head] = val;
        mouse_queue_head = next;
    }
}

/* Pointer events for the Ring-3 compositor: the position the kernel
   computed AFTER each packet, so the compositor never integrates packets
   itself and can't drift from the hardware cursor (which this same IRQ
   drives) -- and the packet's own motion (dx, dy), which goes on at the
   screen's edge and while the pointer is locked: a game's mouse look.
   Only recorded while a reader is attached. */
#define MOUSE_EVENT_RING 256
static MouseEventEx mouse_events[MOUSE_EVENT_RING];
static uint32_t mouse_ev_head = 0, mouse_ev_count = 0;
static bool mouse_ev_enabled = false;

/* the compositor's say over the pointer (SYS_pointer_ctl): locked (it
   stays where it is, only the motion is told), confined to a rectangle,
   or not shown */
static bool ptr_locked = false;
static bool ptr_confined = false;
static int32_t conf_x0, conf_y0, conf_x1, conf_y1;   /* inclusive */
static bool ptr_hidden = false;

/* Interrupts must be off. */
static void mouse_event_push(int32_t dx, int32_t dy)
{
    if (!mouse_ev_enabled) return;
    /* buttons: bits 0-2 the buttons, bits 8-15 this packet's wheel delta
       (int8, positive = towards the user... i.e. scroll down) */
    MouseEventEx ev = { mouse_x, mouse_y, (uint32_t)mouse_buttons | ((uint32_t)(uint8_t)mouse_wheel << 8), dx, dy };
    if (mouse_ev_count == MOUSE_EVENT_RING) {
        /* Full (reader stalled): drop the oldest event whose successor has
           the same buttons, its motion added to that one -- only a
           position is lost, never a press or release. If every event is a
           button change, fold into the newest when this one doesn't change
           buttons, else drop the oldest. */
        uint32_t k;
        for (k = 0; k + 1 < mouse_ev_count; k++) {
            MouseEventEx *a = &mouse_events[(mouse_ev_head + k) % MOUSE_EVENT_RING];
            MouseEventEx *b = &mouse_events[(mouse_ev_head + k + 1) % MOUSE_EVENT_RING];
            if (a->buttons == b->buttons) { b->dx += a->dx; b->dy += a->dy; break; }
        }
        if (k + 1 < mouse_ev_count) {
            for (; k + 1 < mouse_ev_count; k++)
                mouse_events[(mouse_ev_head + k) % MOUSE_EVENT_RING] =
                    mouse_events[(mouse_ev_head + k + 1) % MOUSE_EVENT_RING];
            mouse_ev_count--;
        } else {
            MouseEventEx *last = &mouse_events[(mouse_ev_head + mouse_ev_count - 1) % MOUSE_EVENT_RING];
            if (last->buttons == ev.buttons && !(ev.buttons & 0xFF00)) {
                ev.dx += last->dx; ev.dy += last->dy;
                *last = ev;
                return;
            }
            mouse_ev_head = (mouse_ev_head + 1) % MOUSE_EVENT_RING;
            mouse_ev_count--;
        }
    }
    mouse_events[(mouse_ev_head + mouse_ev_count) % MOUSE_EVENT_RING] = ev;
    mouse_ev_count++;
}

void mouse_events_attach(void)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    mouse_ev_head = mouse_ev_count = 0;
    mouse_ev_enabled = true;
    /* a new owner: nothing of the last one's say over the pointer */
    ptr_locked = ptr_confined = false;
    if (ptr_hidden) {
        extern void virtio_gpu_cursor_hidden(bool hidden);
        ptr_hidden = false;
        virtio_gpu_cursor_hidden(false);
    }
    mouse_event_push(0, 0); /* first event = where the pointer is right now */
    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* ex: MouseEventEx records, else the older MouseEvent (x, y, buttons) */
int mouse_events_read(void *buf, int max, bool ex)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    int n = 0;
    while (n < max && mouse_ev_count > 0) {
        const MouseEventEx *e = &mouse_events[mouse_ev_head];
        if (ex) ((MouseEventEx *)buf)[n] = *e;
        else {
            MouseEvent *o = &((MouseEvent *)buf)[n];
            o->x = e->x; o->y = e->y; o->buttons = e->buttons;
        }
        n++;
        mouse_ev_head = (mouse_ev_head + 1) % MOUSE_EVENT_RING;
        mouse_ev_count--;
    }
    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
    return n;
}

/* One packet from either mouse (dx, dy: right/down): the pointer moved
   within the screen (or the confining rectangle; not at all when locked),
   the event, the cursor plane. Interrupts off. */
static void mouse_apply(int32_t dx, int32_t dy, uint8_t btns, int8_t wheel)
{
    int32_t new_x = mouse_x + dx;
    int32_t new_y = mouse_y + dy;
    int32_t x0 = 0, y0 = 0, x1 = bound_w - 1, y1 = bound_h - 1;
    if (ptr_confined) { x0 = conf_x0; y0 = conf_y0; x1 = conf_x1; y1 = conf_y1; }
    if (ptr_locked) { new_x = mouse_x; new_y = mouse_y; }
    else {
        if (new_x < x0) new_x = x0;
        if (new_x > x1) new_x = x1;
        if (new_y < y0) new_y = y0;
        if (new_y > y1) new_y = y1;
    }

    if (!dx && !dy && btns == mouse_buttons && !wheel) return;
    const bool moved = new_x != mouse_x || new_y != mouse_y;
    mouse_hide();
    mouse_x = new_x;
    mouse_y = new_y;
    mouse_buttons = btns;
    mouse_wheel = wheel;
    mouse_event_push(dx, dy);
    mouse_wheel = 0;   /* one packet's turn: the next move is not a scroll */

    /* Hardware cursor tracks the IRQ directly: moves at input rate no
       matter what the compositor is doing. */
    if (moved && mouse_hw_cursor()) {
        extern void virtio_gpu_move_cursor(uint32_t x, uint32_t y);
        virtio_gpu_move_cursor((uint32_t)mouse_x, (uint32_t)mouse_y);
    }
    mouse_show();
}

/* SYS_pointer_ctl: the compositor's say over the pointer */
void mouse_pointer_lock(bool on)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    ptr_locked = on;
    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
}

void mouse_pointer_confine(bool on, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > bound_w - 1) x1 = bound_w - 1;
    if (y1 > bound_h - 1) y1 = bound_h - 1;
    ptr_confined = on && x0 <= x1 && y0 <= y1;
    conf_x0 = x0; conf_y0 = y0; conf_x1 = x1; conf_y1 = y1;
    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* the pointer to (x, y) (a locked pointer's hint, on its release): no
   event -- the compositor knows */
void mouse_pointer_warp(int32_t x, int32_t y)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > bound_w - 1) x = bound_w - 1;
    if (y > bound_h - 1) y = bound_h - 1;
    mouse_hide();
    mouse_x = x; mouse_y = y;
    if (mouse_hw_cursor()) {
        extern void virtio_gpu_move_cursor(uint32_t x, uint32_t y);
        virtio_gpu_move_cursor((uint32_t)mouse_x, (uint32_t)mouse_y);
    }
    mouse_show();
    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* the cursor plane's picture off (a client asked for no cursor: a game,
   a video player) or on again */
void mouse_pointer_hide(bool hidden)
{
    extern void virtio_gpu_cursor_hidden(bool hidden);
    if (hidden == ptr_hidden) return;
    ptr_hidden = hidden;
    virtio_gpu_cursor_hidden(hidden);
}

int mouse_read_queue(uint8_t *buf, int size)
{
    int read_bytes = 0;
    while (read_bytes < size) {
        if (mouse_queue_head == mouse_queue_tail) {
            break;
        }
        buf[read_bytes++] = mouse_queue[mouse_queue_tail];
        mouse_queue_tail = (mouse_queue_tail + 1) % MOUSE_QUEUE_SIZE;
    }
    return read_bytes;
}

static uint8_t mouse_cycle = 0;
static uint8_t mouse_packet[4];

void mouse_handle_interrupt(uint8_t data)
{
    if (!g_boot_info) return;

    switch (mouse_cycle) {
        case 0:
            if ((data & 0x08) != 0) {
                mouse_packet[0] = data;
                mouse_cycle = 1;
            }
            break;
        case 1:
            mouse_packet[1] = data;
            mouse_cycle = 2;
            break;
        case 2:
        case 3:
            mouse_packet[mouse_cycle] = data;
            if (mouse_has_wheel && mouse_cycle == 2) { mouse_cycle = 3; break; }
            mouse_cycle = 0;
            /* low nibble of the 4th byte, sign-extended */
            mouse_wheel = mouse_has_wheel ? (int8_t)((int8_t)(mouse_packet[3] << 4) >> 4) : 0;

            uint8_t buttons = mouse_packet[0];
            int32_t rel_x = (int32_t)mouse_packet[1];
            int32_t rel_y = (int32_t)mouse_packet[2];

            if (buttons & 0x10) {
                rel_x |= ~0xFF;
            }
            if (buttons & 0x20) {
                rel_y |= ~0xFF;
            }

            mouse_apply(rel_x, -rel_y, buttons & 0x07, mouse_wheel);
            /* Whole packets only: dropping one byte of a full queue would
               shift every packet after it for /dev/input/mice readers. */
            if ((mouse_queue_tail + MOUSE_QUEUE_SIZE - mouse_queue_head - 1) % MOUSE_QUEUE_SIZE >= 3) {
                mouse_queue_push(mouse_packet[0]);
                mouse_queue_push(mouse_packet[1]);
                mouse_queue_push(mouse_packet[2]);
            }
            break;
    }
}

/* A USB HID mouse feeds the same state and queues as the PS/2 one. */
void mouse_inject_usb(int dx, int dy, uint8_t buttons, int wheel)
{
    if (!g_boot_info) return;

    uint8_t btns = buttons & 0x07;
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    mouse_apply(dx, dy, btns, (int8_t)wheel);
    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");

    /* /dev/input/mice speaks PS/2 packets: bit3 always set, y positive up. */
    if ((mouse_queue_tail + MOUSE_QUEUE_SIZE - mouse_queue_head - 1) % MOUSE_QUEUE_SIZE >= 3) {
        mouse_queue_push((uint8_t)(0x08 | btns));
        mouse_queue_push((uint8_t)dx);
        mouse_queue_push((uint8_t)(-dy));
    }
}

/* (a smaller screen: the pointer comes inside it at its next move) */
void mouse_set_bounds(uint32_t w, uint32_t h)
{
    if (!w || !h) return;
    bound_w = (int32_t)w;
    bound_h = (int32_t)h;
    if (mouse_x >= bound_w) mouse_x = bound_w - 1;
    if (mouse_y >= bound_h) mouse_y = bound_h - 1;
}

int32_t mouse_get_x(void)
{
    return mouse_x;
}

int32_t mouse_get_y(void)
{
    return mouse_y;
}

uint8_t mouse_get_buttons(void)
{
    return mouse_buttons;
}
