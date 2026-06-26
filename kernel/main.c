/*
 * WynlandOS - Kernel Main
 * ============================================================
 * Phase 1: Minimal kernel with framebuffer rendering
 *
 * This is the kernel entry point (called from entry.asm).
 * It receives the BootInfo structure from the UEFI bootloader,
 * validates it, and renders a welcome screen to the framebuffer
 * to prove the entire boot pipeline works.
 *
 * Compiled with system gcc (System V ABI) - freestanding, no libc.
 * ============================================================
 */

#include <wynland/types.h>
#include <wynland/boot_info.h>
#include <wynland/idt.h>
#include <wynland/pmm.h>
#include <wynland/vmm.h>
#include <wynland/heap.h>
#include <wynland/irq.h>
#include <wynland/mouse.h>
#include <wynland/sched.h>


#include <wynland/font.h>

/* ============================================================
 * Framebuffer Helpers
 * ============================================================ */

static bool screen_flip = false;

/*
 * fb_put_pixel - Write a single pixel to the framebuffer
 */
static void fb_put_pixel(BootInfo *info, uint32_t x, uint32_t y, uint32_t color)
{
    if (x >= info->fb_width || y >= info->fb_height)
        return;

    uint32_t actual_x = screen_flip ? (info->fb_width - 1 - x) : x;
    uint32_t offset = y * info->fb_pitch + actual_x * (info->fb_bpp / 8);
    uint8_t *fb = (uint8_t *)(uintptr_t)info->fb_addr;
    *((uint32_t *)(fb + offset)) = color;
}

/*
 * fb_fill_rect - Fill a rectangle with a solid color
 */
static UNUSED void fb_fill_rect(BootInfo *info, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h, uint32_t color)
{
    for (uint32_t row = y; row < y + h && row < info->fb_height; row++) {
        for (uint32_t col = x; col < x + w && col < info->fb_width; col++) {
            fb_put_pixel(info, col, row, color);
        }
    }
}

/*
 * fb_draw_char - Draw a single 8x8 character at pixel position (x,y)
 *
 * When bg == 0 (transparent), background pixels are not drawn,
 * allowing the gradient underneath to show through.
 */
#define FONT_SCALE  1
#define CHAR_STEP   (9 * FONT_SCALE)
#define LINE_STEP   (18 * FONT_SCALE)

static void fb_draw_char(BootInfo *info, uint32_t x, uint32_t y,
                          char c, uint32_t fg, uint32_t bg)
{
    const uint8_t *glyph = font_8x16[(uint8_t)c];

    for (uint32_t row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < 8; col++) {
            /* MSB is leftmost pixel: test bit 7-col */
            if (bits & (0x80 >> col)) {
                if (FONT_SCALE == 1) {
                    fb_put_pixel(info, x + col, y + row, fg);
                } else {
                    for (uint32_t sy = 0; sy < FONT_SCALE; sy++) {
                        for (uint32_t sx = 0; sx < FONT_SCALE; sx++) {
                            fb_put_pixel(info, x + col * FONT_SCALE + sx, y + row * FONT_SCALE + sy, fg);
                        }
                    }
                }
            } else if (bg != 0) {
                if (FONT_SCALE == 1) {
                    fb_put_pixel(info, x + col, y + row, bg);
                } else {
                    for (uint32_t sy = 0; sy < FONT_SCALE; sy++) {
                        for (uint32_t sx = 0; sx < FONT_SCALE; sx++) {
                            fb_put_pixel(info, x + col * FONT_SCALE + sx, y + row * FONT_SCALE + sy, bg);
                        }
                    }
                }
            }
        }
    }
}

/*
 * fb_draw_string - Draw a null-terminated string
 */
static void fb_draw_string(BootInfo *info, uint32_t x, uint32_t y,
                            const char *str, uint32_t fg, uint32_t bg)
{
    uint32_t start_x = x;

    while (*str) {
        if (*str == '\n') {
            x = start_x;
            y += LINE_STEP;
        } else {
            fb_draw_char(info, x, y, *str, fg, bg);
            x += CHAR_STEP;
        }
        str++;
    }
}

/*
 * fb_draw_gradient - Fill the entire screen with a dark gradient
 *
 * Top:    dark blue  (#0A0A2E)
 * Bottom: dark purple (#1A0A38)
 * Creates a smooth, visually appealing background.
 */
static void fb_draw_gradient(BootInfo *info)
{
    uint32_t w = info->fb_width;
    uint32_t h = info->fb_height;
    uint8_t *fb = (uint8_t *)(uintptr_t)info->fb_addr;

    for (uint32_t y = 0; y < h; y++) {
        /* Interpolate RGB values from top to bottom */
        uint8_t r = (uint8_t)(10 + (y * 16 / h));   /* 0x0A -> 0x1A */
        uint8_t g = 10;                                /* constant 0x0A */
        uint8_t b = (uint8_t)(46 + (y * 10 / h));   /* 0x2E -> 0x38 */

        /* Pack as BGRA (most common UEFI framebuffer format) */
        uint32_t color = ((uint32_t)b) | ((uint32_t)g << 8) |
                         ((uint32_t)r << 16) | (0xFF000000u);

        /* Fill the entire row */
        uint32_t *row_ptr = (uint32_t *)(fb + y * info->fb_pitch);
        for (uint32_t x = 0; x < w; x++) {
            row_ptr[x] = color;
        }
    }
}

/* ============================================================
 * Number-to-String Helpers (freestanding, no libc)
 * ============================================================ */

/*
 * uint_to_str - Convert a uint64_t to a decimal string
 */
void uint_to_str(uint64_t value, char *buf)
{
    char tmp[21]; /* Max uint64 is 20 digits + null */
    int i = 0;

    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }

    while (value > 0) {
        tmp[i++] = '0' + (char)(value % 10);
        value /= 10;
    }

    /* Reverse into output buffer */
    int j = 0;
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
}

/*
 * uint_to_hex - Convert a uint64_t to a hex string with "0x" prefix
 */
void uint_to_hex(uint64_t value, char *buf)
{
    const char hex_chars[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';

    if (value == 0) {
        buf[2] = '0';
        buf[3] = '\0';
        return;
    }

    /* Find the highest non-zero nibble */
    int start = 60; /* Start from the highest nibble (bits 63-60) */
    while (start >= 0 && ((value >> start) & 0xF) == 0) {
        start -= 4;
    }

    int pos = 2;
    while (start >= 0) {
        buf[pos++] = hex_chars[(value >> start) & 0xF];
        start -= 4;
    }
    buf[pos] = '\0';
}

/* ============================================================
 * Simple string helpers
 * ============================================================ */

/*
 * str_len - Get the length of a null-terminated string
 */
static uint32_t str_len(const char *s)
{
    uint32_t len = 0;
    while (*s++) len++;
    return len;
}

/*
 * str_copy - Copy src string into dst buffer
 */
static void str_copy(char *dst, const char *src)
{
    while (*src)
        *dst++ = *src++;
    *dst = '\0';
}

/*
 * str_append - Append src to the end of dst
 */
static UNUSED void str_append(char *dst, const char *src)
{
    while (*dst) dst++;
    str_copy(dst, src);
}

/* ============================================================
 * Port I/O and Memory Helpers
 * ============================================================ */

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

static inline void outw(uint16_t port, uint16_t data)
{
    __asm__ volatile("outw %0, %1" :: "a"(data), "Nd"(port));
}

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    void *orig = dest;
    __asm__ volatile("rep movsb" : "+D"(dest), "+S"(src), "+c"(n) : : "memory");
    return orig;
}

void *memset(void *s, int c, size_t n)
{
    void *orig = s;
    __asm__ volatile("rep stosb" : "+D"(s), "+c"(n) : "a"(c) : "memory");
    return orig;
}

static int str_compare(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

/* ============================================================
 * Terminal Window and Custom Graphics
 * ============================================================ */

static uint32_t term_x;
static uint32_t term_y;
static uint32_t term_w;
static uint32_t term_h;
uint32_t console_start_x;
uint32_t console_start_y;
uint32_t console_end_x;
uint32_t console_end_y;
static uint32_t term_bg_color = 0x000F0F1A; /* Catppuccin Mocha style dark background */

uint32_t cursor_x = 0;
uint32_t cursor_y = 0;

static void draw_circle(BootInfo *info, uint32_t cx, uint32_t cy, uint32_t r, uint32_t color)
{
    for (uint32_t y = cy - r; y <= cy + r; y++) {
        for (uint32_t x = cx - r; x <= cx + r; x++) {
            uint32_t dx = (x > cx) ? (x - cx) : (cx - x);
            uint32_t dy = (y > cy) ? (y - cy) : (cy - y);
            if (dx * dx + dy * dy <= r * r) {
                fb_put_pixel(info, x, y, color);
            }
        }
    }
}

static void draw_terminal_window(BootInfo *info)
{
    mouse_hide();
    /* Terminal window parameters - sized dynamically to screen resolution */
    term_w = info->fb_width * 4 / 5;
    term_h = info->fb_height * 3 / 4;
    term_x = (info->fb_width - term_w) / 2;
    term_y = (info->fb_height - term_h) / 2;
    
    console_start_x = term_x + 15;
    console_start_y = term_y + 45;
    console_end_x = term_x + term_w - 15;
    console_end_y = term_y + term_h - 15;
    
    /* Draw window background & title bar */
    for (uint32_t y = term_y; y < term_y + term_h; y++) {
        uint32_t *row_ptr = (uint32_t *)((uint8_t *)(uintptr_t)info->fb_addr + y * info->fb_pitch);
        for (uint32_t x = term_x; x < term_x + term_w; x++) {
            if (y < term_y + 30) {
                row_ptr[x] = 0x001A1A26; /* Title bar color */
            } else {
                row_ptr[x] = term_bg_color; /* Window content background */
            }
        }
    }
    
    /* Draw window border */
    uint32_t border_color = 0x00886EFF; /* Purple accent */
    for (uint32_t x = term_x; x < term_x + term_w; x++) {
        *((uint32_t *)((uint8_t *)(uintptr_t)info->fb_addr + term_y * info->fb_pitch + x * 4)) = border_color;
        *((uint32_t *)((uint8_t *)(uintptr_t)info->fb_addr + (term_y + term_h - 1) * info->fb_pitch + x * 4)) = border_color;
    }
    for (uint32_t y = term_y; y < term_y + term_h; y++) {
        *((uint32_t *)((uint8_t *)(uintptr_t)info->fb_addr + y * info->fb_pitch + term_x * 4)) = border_color;
        *((uint32_t *)((uint8_t *)(uintptr_t)info->fb_addr + y * info->fb_pitch + (term_x + term_w - 1) * 4)) = border_color;
    }
    
    /* Draw macOS-style window controls */
    draw_circle(info, term_x + 15, term_y + 15, 6, 0x00FF5F56); /* Red */
    draw_circle(info, term_x + 35, term_y + 15, 6, 0x00FFBD2E); /* Yellow */
    draw_circle(info, term_x + 55, term_y + 15, 6, 0x0027C93F); /* Green */
    
    /* Draw title text centered */
    const char *title = "WynlandOS Terminal - Shell v0.1";
    uint32_t title_len = str_len(title);
    uint32_t title_start_x = term_x + (term_w - title_len * CHAR_STEP) / 2;
    fb_draw_string(info, title_start_x, term_y + 7, title, 0x00FFFFFF, 0x001A1A26);
    mouse_show();
}

/* ============================================================
 * Serial Port (COM1) Driver for Debug logging
 * ============================================================ */

#define PORT_COM1 0x3F8

static void serial_init(void)
{
    outb(PORT_COM1 + 1, 0x00);    /* Disable all interrupts */
    outb(PORT_COM1 + 3, 0x80);    /* Enable DLAB (set baud rate divisor) */
    outb(PORT_COM1 + 0, 0x03);    /* Set divisor to 3 (lo byte) 38400 baud */
    outb(PORT_COM1 + 1, 0x00);    /*                  (hi byte) */
    outb(PORT_COM1 + 3, 0x03);    /* 8 bits, no parity, one stop bit */
    outb(PORT_COM1 + 2, 0xC7);    /* Enable FIFO, clear them, 14-byte threshold */
    outb(PORT_COM1 + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */
}

static int serial_is_transmit_empty(void)
{
    return inb(PORT_COM1 + 5) & 0x20;
}

static void serial_write_char(char c)
{
    while (serial_is_transmit_empty() == 0);
    outb(PORT_COM1, c);
}

void serial_write_string(const char *str)
{
    while (*str) {
        if (*str == '\n') {
            serial_write_char('\r');
        }
        serial_write_char(*str);
        str++;
    }
}

static bool serial_received(void)
{
    return inb(PORT_COM1 + 5) & 1;
}

static char serial_read_char(void)
{
    return inb(PORT_COM1);
}


/* ============================================================
 * Terminal Console Text Engine
 * ============================================================ */

static void console_scroll(BootInfo *info)
{
    uint32_t bpp = info->fb_bpp / 8;
    uint32_t bytes_per_line = info->fb_pitch;
    uint8_t *fb = (uint8_t *)(uintptr_t)info->fb_addr;
    
    /* Shift terminal content rows up by LINE_STEP pixels */
    uint32_t copy_height = console_end_y - console_start_y - LINE_STEP;
    for (uint32_t y = 0; y < copy_height; y++) {
        uint32_t dst_y = console_start_y + y;
        uint32_t src_y = console_start_y + y + LINE_STEP;
        memcpy(fb + dst_y * bytes_per_line + console_start_x * bpp,
               fb + src_y * bytes_per_line + console_start_x * bpp,
               (console_end_x - console_start_x) * bpp);
    }
    
    /* Clear bottom row */
    uint32_t clear_y = console_end_y - LINE_STEP;
    for (uint32_t y = 0; y < LINE_STEP; y++) {
        uint32_t *row_ptr = (uint32_t *)(fb + (clear_y + y) * bytes_per_line);
        for (uint32_t x = console_start_x; x < console_end_x; x++) {
            row_ptr[x] = term_bg_color;
        }
    }
}

static void console_clear_current_line(BootInfo *info)
{
    uint32_t y_start = console_start_y + cursor_y * LINE_STEP;
    uint8_t *fb = (uint8_t *)(uintptr_t)info->fb_addr;
    for (uint32_t y = 0; y < LINE_STEP; y++) {
        uint32_t *row_ptr = (uint32_t *)(fb + (y_start + y) * info->fb_pitch);
        for (uint32_t x = console_start_x; x < console_end_x; x++) {
            row_ptr[x] = term_bg_color;
        }
    }
    cursor_x = 0;
}

static bool disable_serial_mirror = false;

void console_print_char(BootInfo *info, char c, uint32_t fg, uint32_t bg)
{
    /* Mirror output to COM1 serial port */
    if (!disable_serial_mirror) {
        if (c == '\n') {
            serial_write_char('\r');
            serial_write_char('\n');
        } else if (c == '\b') {
            serial_write_char('\b');
            serial_write_char(' ');
            serial_write_char('\b');
        } else {
            serial_write_char(c);
        }
    }

    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (console_start_y + cursor_y * LINE_STEP >= console_end_y) {
            console_scroll(info);
            cursor_y--;
        }
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            fb_draw_char(info, console_start_x + cursor_x * CHAR_STEP, console_start_y + cursor_y * LINE_STEP, ' ', fg, bg);
        }
    } else {
        fb_draw_char(info, console_start_x + cursor_x * CHAR_STEP, console_start_y + cursor_y * LINE_STEP, c, fg, bg);
        cursor_x++;
        if (console_start_x + cursor_x * CHAR_STEP >= console_end_x) {
            cursor_x = 0;
            cursor_y++;
            if (console_start_y + cursor_y * LINE_STEP >= console_end_y) {
                console_scroll(info);
                cursor_y--;
            }
        }
    }
}

void console_print_string(BootInfo *info, const char *str, uint32_t fg, uint32_t bg)
{
    mouse_hide();
    while (*str) {
        console_print_char(info, *str, fg, bg);
        str++;
    }
    mouse_show();
}

/* ============================================================
 * Autocomplete Suggestion Logic & Redrawing
 * ============================================================ */

static const char scancode_to_ascii_lower[59] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

static const char scancode_to_ascii_upper[59] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

static const char* find_suggestion(const char *prefix, int len)
{
    if (len == 0) return NULL;
    const char* commands[] = {"help", "clear", "about", "mmap", "neofetch", "tasks", "reboot", "poweroff", "panic", "heap_test"};
    int cmd_count = 10;
    for (int i = 0; i < cmd_count; i++) {
        bool match = true;
        for (int j = 0; j < len; j++) {
            if (prefix[j] == '\0' || commands[i][j] != prefix[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            return commands[i];
        }
    }
    return NULL;
}

static void draw_input_line(BootInfo *info, const char *prompt, const char *input, const char *suggestion)
{
    mouse_hide();
    disable_serial_mirror = true;

    /* Clear line, then print prompt + user text */
    console_clear_current_line(info);
    console_print_string(info, prompt, 0x00886EFF, term_bg_color);
    console_print_string(info, input, 0x00FFFFFF, term_bg_color);
    
    uint32_t cur_x = cursor_x;
    uint32_t cur_y = cursor_y;
    
    /* Draw inline autocomplete suggestion in dim color */
    if (suggestion && suggestion[0] != '\0') {
        int input_len = str_len(input);
        const char *suffix = suggestion + input_len;
        console_print_string(info, suffix, 0x00555577, term_bg_color);
    }
    
    /* Reset cursor back to end of user input and render cursor block */
    cursor_x = cur_x;
    cursor_y = cur_y;
    fb_draw_char(info, console_start_x + cursor_x * CHAR_STEP, console_start_y + cursor_y * LINE_STEP, '_', 0x0000FF00, term_bg_color);

    disable_serial_mirror = false;
    mouse_show();
}

/* ============================================================
 * Hardware Reset Commands
 * ============================================================ */

static void reboot(void)
{
    /* 1. Pulse CPU reset via keyboard controller (port 0x64) */
    uint8_t temp = 0x02;
    while (temp & 0x02) {
        temp = inb(0x64);
    }
    outb(0x64, 0xFE);
    
    /* 2. Fallback: Trigger triple fault by loading an invalid IDT */
    __asm__ volatile("lidt %0; int3" :: "m"((uint16_t[3]){0, 0, 0}));
}

static void poweroff(void)
{
    /* QEMU q35 ACPI shutdown */
    outw(0x604, 0x2000);
    /* QEMU piix4 ACPI shutdown */
    outw(0xB004, 0x2000);
    /* VirtualBox / older QEMU ACPI shutdown */
    outw(0x4004, 0x3400);
}

static volatile int test_thread_1_done = 0;
static volatile int test_thread_2_done = 0;

static void test_thread_1(void *arg) {
    (void)arg;
    for (int i = 1; i <= 5; i++) {
        serial_write_string("Thread 1: Running...\r\n");
        for (volatile int d = 0; d < 20000000; d++);
    }
    serial_write_string("Thread 1: Terminating\r\n");
    test_thread_1_done = 1;
}

static void test_thread_2(void *arg) {
    (void)arg;
    for (int i = 1; i <= 5; i++) {
        serial_write_string("Thread 2: Running...\r\n");
        for (volatile int d = 0; d < 20000000; d++);
    }
    serial_write_string("Thread 2: Terminating\r\n");
    test_thread_2_done = 1;
}

/* ============================================================
 * Command Executor
 * ============================================================ */

static void execute_command(BootInfo *info, const char *cmd)
{
    if (str_compare(cmd, "help") == 0) {
        console_print_string(info, "Available commands:\n", 0x0000FF00, term_bg_color);
        console_print_string(info, "  help       - Show this help message\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  clear      - Clear the terminal screen\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  about      - Show system information\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  mmap       - Print the memory map\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  neofetch   - Show OS logo and specs\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  tasks      - Show active system tasks\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  reboot     - Reboot the system\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  poweroff   - Shut down the PC\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  panic      - Trigger a test CPU exception\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  heap_test  - Run dynamic memory allocation test\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  sched_test - Run multitasking scheduler test\n", 0x00FFFFFF, term_bg_color);
    } else if (str_compare(cmd, "tasks") == 0) {
        sched_print_tasks(info, term_bg_color);
    } else if (str_compare(cmd, "sched_test") == 0) {
        console_print_string(info, "Running multitasking scheduler test...\n", 0x00FFFF00, term_bg_color);
        test_thread_1_done = 0;
        test_thread_2_done = 0;

        thread_create(test_thread_1, NULL);
        thread_create(test_thread_2, NULL);

        while (!test_thread_1_done || !test_thread_2_done) {
            sched_yield();
        }
        console_print_string(info, "Multitasking test completed successfully!\n", 0x0000FF00, term_bg_color);
    } else if (str_compare(cmd, "clear") == 0) {
        for (uint32_t y = console_start_y; y < console_end_y; y++) {
            uint32_t *row_ptr = (uint32_t *)((uint8_t *)(uintptr_t)info->fb_addr + y * info->fb_pitch);
            for (uint32_t x = console_start_x; x < console_end_x; x++) {
                row_ptr[x] = term_bg_color;
            }
        }
        cursor_x = 0;
        cursor_y = 0;
    } else if (str_compare(cmd, "about") == 0) {
        console_print_string(info, "WynlandOS v0.1\n", 0x0000FFFF, term_bg_color);
        console_print_string(info, "===============================\n", 0x00888888, term_bg_color);
        
        console_print_string(info, "Framebuffer physical address: ", 0x00CCCCCC, term_bg_color);
        char buf[64];
        uint_to_hex(info->fb_addr, buf);
        console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n", 0x00FFFFFF, term_bg_color);
        
        console_print_string(info, "Resolution: ", 0x00CCCCCC, term_bg_color);
        uint_to_str(info->fb_width, buf);
        console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
        console_print_string(info, "x", 0x00CCCCCC, term_bg_color);
        uint_to_str(info->fb_height, buf);
        console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n", 0x00FFFFFF, term_bg_color);
        
        console_print_string(info, "ACPI RSDP Table address: ", 0x00CCCCCC, term_bg_color);
        uint_to_hex(info->rsdp_addr, buf);
        console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n", 0x00FFFFFF, term_bg_color);
        
        console_print_string(info, "Kernel Physical Base: ", 0x00CCCCCC, term_bg_color);
        uint_to_hex(info->kernel_phys, buf);
        console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n", 0x00FFFFFF, term_bg_color);
        
        console_print_string(info, "\nPhysical Memory (PMM):\n", 0x0000FF00, term_bg_color);
        console_print_string(info, "  Total memory: ", 0x00CCCCCC, term_bg_color);
        uint_to_str(pmm_get_total_memory() / (1024 * 1024), buf);
        console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
        console_print_string(info, " MB\n", 0x00CCCCCC, term_bg_color);
        
        console_print_string(info, "  Used memory:  ", 0x00CCCCCC, term_bg_color);
        uint_to_str(pmm_get_used_memory() / 1024, buf);
        console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
        console_print_string(info, " KB\n", 0x00CCCCCC, term_bg_color);
        
        console_print_string(info, "  Free memory:  ", 0x00CCCCCC, term_bg_color);
        uint_to_str(pmm_get_free_memory() / (1024 * 1024), buf);
        console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
        console_print_string(info, " MB\n", 0x00CCCCCC, term_bg_color);

        console_print_string(info, "\nKernel Heap (malloc):\n", 0x0000FF00, term_bg_color);
        console_print_string(info, "  Used heap:    ", 0x00CCCCCC, term_bg_color);
        uint_to_str(heap_get_used_memory() / 1024, buf);
        console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
        console_print_string(info, " KB\n", 0x00CCCCCC, term_bg_color);

        console_print_string(info, "  Free heap:    ", 0x00CCCCCC, term_bg_color);
        uint_to_str(heap_get_free_memory() / 1024, buf);
        console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
        console_print_string(info, " KB\n", 0x00CCCCCC, term_bg_color);
    } else if (str_compare(cmd, "mmap") == 0) {
        console_print_string(info, "UEFI Memory Map:\n", 0x0000FFFF, term_bg_color);
        console_print_string(info, "Base Address       | Size               | Type\n", 0x00CCCCCC, term_bg_color);
        console_print_string(info, "-------------------|--------------------|-----------\n", 0x00888888, term_bg_color);
        
        MemoryRegion *regions = (MemoryRegion*)(uintptr_t)info->mmap_addr;
        for (uint32_t i = 0; i < info->mmap_entries && i < 15; i++) {
            char buf[64];
            uint_to_hex(regions[i].base, buf);
            console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
            int pad = 19 - str_len(buf);
            while (pad-- > 0) console_print_char(info, ' ', 0, term_bg_color);
            console_print_string(info, "| ", 0x00888888, term_bg_color);
            
            uint_to_hex(regions[i].size, buf);
            console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
            pad = 19 - str_len(buf);
            while (pad-- > 0) console_print_char(info, ' ', 0, term_bg_color);
            console_print_string(info, "| ", 0x00888888, term_bg_color);
            
            switch (regions[i].type) {
                case MEMORY_USABLE:           console_print_string(info, "Usable\n", 0x0000FF00, term_bg_color); break;
                case MEMORY_RESERVED:         console_print_string(info, "Reserved\n", 0x00FF0000, term_bg_color); break;
                case MEMORY_ACPI_RECLAIMABLE: console_print_string(info, "ACPI Reclaim\n", 0x00FFFF00, term_bg_color); break;
                case MEMORY_ACPI_NVS:         console_print_string(info, "ACPI NVS\n", 0x00FFFF00, term_bg_color); break;
                case MEMORY_BOOTLOADER:       console_print_string(info, "Bootloader\n", 0x00FF8800, term_bg_color); break;
                case MEMORY_KERNEL:           console_print_string(info, "Kernel\n", 0x008888FF, term_bg_color); break;
                default:                      console_print_string(info, "Unknown\n", 0x00888888, term_bg_color); break;
            }
        }
        if (info->mmap_entries > 15) {
            console_print_string(info, "... and ", 0x00888888, term_bg_color);
            char buf[16];
            uint_to_str(info->mmap_entries - 15, buf);
            console_print_string(info, buf, 0x00888888, term_bg_color);
            console_print_string(info, " more regions.\n", 0x00888888, term_bg_color);
        }
    } else if (str_compare(cmd, "neofetch") == 0) {
        console_print_string(info, "      /\\_ /\\       wynland@wynlandos\n", 0x0000FFFF, term_bg_color);
        console_print_string(info, "     (  o.o  )      -----------------\n", 0x0000FFFF, term_bg_color);
        console_print_string(info, "      >  ^  <       OS: WynlandOS v0.1 x64\n", 0x0000FFFF, term_bg_color);
        console_print_string(info, "     /   *   \\      Kernel: Freestanding Monolithic (Phase 2)\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "    /    |    \\     Shell: WynlandShell v0.1\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "   (____/ \\____)    Resolution: ", 0x00FFFFFF, term_bg_color);
        
        char buf[32];
        uint_to_str(info->fb_width, buf);
        console_print_string(info, buf, 0x0000FF00, term_bg_color);
        console_print_string(info, "x", 0x00FFFFFF, term_bg_color);
        uint_to_str(info->fb_height, buf);
        console_print_string(info, buf, 0x0000FF00, term_bg_color);
        console_print_string(info, "\n", 0x00FFFFFF, term_bg_color);
        
        console_print_string(info, "                    Memory: ", 0x00FFFFFF, term_bg_color);
        uint_to_str(info->total_memory / (1024 * 1024), buf);
        console_print_string(info, buf, 0x0000FF00, term_bg_color);
        console_print_string(info, " MB usable\n", 0x00FFFFFF, term_bg_color);
    } else if (str_compare(cmd, "reboot") == 0) {
        console_print_string(info, "Rebooting system...\n", 0x00FF0000, term_bg_color);
        for (volatile int i = 0; i < 50000000; i++);
        reboot();
    } else if (str_compare(cmd, "poweroff") == 0) {
        console_print_string(info, "Shutting down...\n", 0x00FF0000, term_bg_color);
        for (volatile int i = 0; i < 50000000; i++);
        poweroff();
    } else if (str_compare(cmd, "panic") == 0) {
        console_print_string(info, "Triggering test CPU exception...\n", 0x00FF0000, term_bg_color);
        for (volatile int i = 0; i < 20000000; i++);
        __asm__ volatile("ud2"); /* Trigger an Invalid Opcode (#UD) exception */
    } else if (str_compare(cmd, "heap_test") == 0) {
        console_print_string(info, "Running heap allocator test...\n", 0x00FFFF00, term_bg_color);
        void *p1 = kmalloc(128);
        void *p2 = kmalloc(512);
        void *p3 = kmalloc(1024 * 16); /* 16 KB (forces heap growth) */

        if (p1 && p2 && p3) {
            console_print_string(info, "  All blocks allocated successfully!\n", 0x0000FF00, term_bg_color);
            char hex_buf[32];
            console_print_string(info, "  p1 addr (128B):  ", 0x00CCCCCC, term_bg_color);
            uint_to_hex((uint64_t)p1, hex_buf);
            console_print_string(info, hex_buf, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0x00FFFFFF, term_bg_color);

            console_print_string(info, "  p2 addr (512B):  ", 0x00CCCCCC, term_bg_color);
            uint_to_hex((uint64_t)p2, hex_buf);
            console_print_string(info, hex_buf, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0x00FFFFFF, term_bg_color);

            console_print_string(info, "  p3 addr (16KB):  ", 0x00CCCCCC, term_bg_color);
            uint_to_hex((uint64_t)p3, hex_buf);
            console_print_string(info, hex_buf, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0x00FFFFFF, term_bg_color);

            kfree(p1);
            kfree(p2);
            kfree(p3);
            console_print_string(info, "  All blocks freed successfully!\n", 0x0000FF00, term_bg_color);
        } else {
            console_print_string(info, "  Heap allocation FAILED!\n", 0x00FF0000, term_bg_color);
        }
    } else if (str_len(cmd) > 0) {
        console_print_string(info, "wynland: command not found: ", 0x00FF0000, term_bg_color);
        console_print_string(info, cmd, 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\nTry 'help' for a list of commands.\n", 0x00CCCCCC, term_bg_color);
    }
}

/* ============================================================
 * Kernel Main Entry Point
 * ============================================================ */

void kernel_main(BootInfo *boot_info)
{
    /* ---- Validate the boot info magic ---- */
    if (!boot_info || boot_info->magic != WYNLAND_BOOT_MAGIC) {
        while (1) {
            __asm__ volatile("hlt");
        }
    }

    /* ---- Initialize GDT ---- */
    // GDT initialized in entry.asm

    /* ---- Initialize IDT ---- */
    idt_init(boot_info);

    /* ---- Initialize PMM ---- */
    pmm_init(boot_info);

    /* ---- Initialize VMM ---- */
    vmm_init(boot_info);

    /* ---- Initialize Heap ---- */
    heap_init();

    /* ---- Initialize Scheduler ---- */
    sched_init();

    /* ---- Initialize IRQs ---- */
    irq_init();

    /* ---- Initialize Mouse ---- */
    mouse_init(boot_info);

    /* ---- Initialize Serial Port (COM1) ---- */
    serial_init();
    serial_write_string("\r\nWynlandOS Kernel Starting...\r\n");

    /* ---- Draw gradient background ---- */
    fb_draw_gradient(boot_info);

    /* ---- Draw Terminal Window ---- */
    draw_terminal_window(boot_info);

    cursor_x = 0;
    cursor_y = 0;

    char input_buf[64];
    int input_len = 0;
    input_buf[0] = '\0';
    bool shift_pressed = false;
    bool needs_redraw = true;
    const char *prompt = "wynland ~ ";

    /* Drain keyboard controller buffer from BIOS/UEFI legacy inputs with timeout */
    int drain_timeout = 1000;
    while ((inb(0x64) & 1) && drain_timeout > 0) {
        uint8_t val = inb(0x60);
        if (val == 0xFF) {
            break; /* No keyboard controller or error */
        }
        drain_timeout--;
    }

    /* Print shell header info */
    console_print_string(boot_info, "WynlandShell v0.1 initialized.\n", 0x0000FF00, term_bg_color);
    console_print_string(boot_info, "Type 'help' to see available commands.\n\n", 0x0000FFFF, term_bg_color);
    
    /* Draw initial prompt */
    console_print_string(boot_info, prompt, 0x00886EFF, term_bg_color);

    while (1) {
        uint8_t sc = 0;
        char serial_char = 0;
        if (keyboard_has_scancode()) {
            sc = keyboard_pop_scancode();
        } else if (serial_received()) {
            serial_char = serial_read_char();
        } else {
            /* Put CPU to sleep until next interrupt */
            __asm__ volatile("hlt");
            continue;
        }

        if (serial_char != 0) {
            if (serial_char == '\r' || serial_char == '\n') {
                draw_input_line(boot_info, prompt, input_buf, NULL);
                console_print_string(boot_info, "\n", 0xFFFFFFFF, term_bg_color);
                
                mouse_hide();
                execute_command(boot_info, input_buf);
                mouse_show();
                
                input_buf[0] = '\0';
                input_len = 0;
                
                console_print_string(boot_info, prompt, 0x00886EFF, term_bg_color);
                needs_redraw = true;
            } else if (serial_char == '\b' || serial_char == 127) {
                if (input_len > 0) {
                    input_len--;
                    input_buf[input_len] = '\0';
                    needs_redraw = true;
                }
            } else if (serial_char >= 32 && serial_char <= 126 && input_len < 60) {
                input_buf[input_len++] = serial_char;
                input_buf[input_len] = '\0';
                needs_redraw = true;
            }
            continue;
        }

        if (sc == 0xE0) {
            /* Handle extended scancodes (arrows, etc.) by waiting for next queue entry */
            int timeout = 100000;
            while (!keyboard_has_scancode() && timeout > 0) {
                timeout--;
                __asm__ volatile("nop");
            }
            uint8_t sc2 = keyboard_has_scancode() ? keyboard_pop_scancode() : 0;
            if (sc2 != 0 && !(sc2 & 0x80)) {
                if (sc2 == 0x4D) { /* Right Arrow: Autocomplete */
                    const char *sug = find_suggestion(input_buf, input_len);
                    if (sug) {
                        str_copy(input_buf, sug);
                        input_len = str_len(input_buf);
                        needs_redraw = true;
                    }
                }
            }
        } else if (sc > 0) {
            if (sc & 0x80) {
                /* Key release */
                uint8_t released_sc = sc & 0x7F;
                if (released_sc == 0x2A || released_sc == 0x36) {
                    shift_pressed = false;
                }
            } else {
                /* Key press */
                if (sc == 0x2A || sc == 0x36) {
                    shift_pressed = true;
                } else if (sc == 0x0E) { /* Backspace */
                    if (input_len > 0) {
                        input_len--;
                        input_buf[input_len] = '\0';
                        needs_redraw = true;
                    }
                } else if (sc == 0x0F) { /* Tab: Autocomplete */
                    const char *sug = find_suggestion(input_buf, input_len);
                    if (sug) {
                        str_copy(input_buf, sug);
                        input_len = str_len(input_buf);
                        needs_redraw = true;
                    }
                } else if (sc == 0x1C) { /* Enter */
                    /* Draw final input text without suggestion gray suffix */
                    draw_input_line(boot_info, prompt, input_buf, NULL);
                    console_print_string(boot_info, "\n", 0xFFFFFFFF, term_bg_color);
                    
                    mouse_hide();
                    execute_command(boot_info, input_buf);
                    mouse_show();
                    
                    input_buf[0] = '\0';
                    input_len = 0;
                    
                    console_print_string(boot_info, prompt, 0x00886EFF, term_bg_color);
                    needs_redraw = true;
                } else {
                    if (sc < 59) {
                        char ascii = shift_pressed ? scancode_to_ascii_upper[sc] : scancode_to_ascii_lower[sc];
                        if (ascii >= 32 && ascii <= 126 && input_len < 60) {
                            input_buf[input_len++] = ascii;
                            input_buf[input_len] = '\0';
                            needs_redraw = true;
                        }
                    }
                }
            }
        }

        if (needs_redraw) {
            const char *sug = find_suggestion(input_buf, input_len);
            draw_input_line(boot_info, prompt, input_buf, sug);
            needs_redraw = false;
        }
    }
}

