/*
 * WynlandOS - PS/2 Mouse Driver Implementation
 */

#include <wynland/mouse.h>

#define CURSOR_W 12
#define CURSOR_H 12

static const uint8_t cursor_mask[12][12] = {
    { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 1, 1, 1, 1, 1, 0, 0, 0 },
    { 1, 2, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0 },
    { 1, 1, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0 }
};

static BootInfo *g_boot_info = NULL;
static int32_t mouse_x = 0;
static int32_t mouse_y = 0;
static uint8_t mouse_buttons = 0;


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
    uint32_t *fb = (uint32_t *)(uintptr_t)g_boot_info->fb_addr;
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
    uint32_t *fb = (uint32_t *)(uintptr_t)g_boot_info->fb_addr;
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
    uint32_t *fb = (uint32_t *)(uintptr_t)g_boot_info->fb_addr;
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

void mouse_hide(void)
{
    if (mouse_hide_count == 0 && cursor_visible) {
        mouse_restore_background();
        cursor_visible = false;
    }
    mouse_hide_count++;
}

void mouse_show(void)
{
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

static uint8_t mouse_cycle = 0;
static uint8_t mouse_packet[3];

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
            mouse_packet[2] = data;
            mouse_cycle = 0;

            uint8_t buttons = mouse_packet[0];
            int32_t rel_x = (int32_t)mouse_packet[1];
            int32_t rel_y = (int32_t)mouse_packet[2];

            if (buttons & 0x10) {
                rel_x |= ~0xFF;
            }
            if (buttons & 0x20) {
                rel_y |= ~0xFF;
            }

            int32_t new_x = mouse_x + rel_x;
            int32_t new_y = mouse_y - rel_y;

            if (new_x < 0) new_x = 0;
            if (new_x >= (int32_t)g_boot_info->fb_width) new_x = g_boot_info->fb_width - 1;
            if (new_y < 0) new_y = 0;
            if (new_y >= (int32_t)g_boot_info->fb_height) new_y = g_boot_info->fb_height - 1;

            if (new_x != mouse_x || new_y != mouse_y || ((buttons & 0x07) != mouse_buttons)) {
                mouse_hide();
                mouse_x = new_x;
                mouse_y = new_y;
                mouse_buttons = buttons & 0x07;
                mouse_show();
            }
            break;
    }
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
