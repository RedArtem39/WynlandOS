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
#include <wynland/vfs.h>
#include <wynland/wlang.h>
#include <wynland/net.h>
#include <wynland/tcp.h>
#include <wynland/http.h>
#include <wynland/wynpkg.h>

/* GUI Compositor */
extern void compositor_init(BootInfo *info);
extern void compositor_flip(void);
extern void comp_draw_wallpaper(void);
extern void comp_draw_panel(void);
extern void comp_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
extern void comp_fill_rect_alpha(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb);
extern void comp_draw_string(uint32_t x, uint32_t y, const char *str, uint32_t fg, uint32_t bg);
extern void comp_draw_circle(uint32_t cx, uint32_t cy, uint32_t r, uint32_t color);
extern void comp_draw_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t color);
extern void comp_mark_dirty(void);
extern uint32_t comp_get_width(void);
extern uint32_t comp_get_height(void);

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

static void get_braille_glyph(uint8_t mask, uint8_t *glyph)
{
    for (int i = 0; i < 16; i++) {
        glyph[i] = 0;
    }
    if (mask & 0x01) { glyph[2] |= 0x20; glyph[3] |= 0x20; }
    if (mask & 0x02) { glyph[6] |= 0x20; glyph[7] |= 0x20; }
    if (mask & 0x04) { glyph[10] |= 0x20; glyph[11] |= 0x20; }
    if (mask & 0x08) { glyph[2] |= 0x04; glyph[3] |= 0x04; }
    if (mask & 0x10) { glyph[6] |= 0x04; glyph[7] |= 0x04; }
    if (mask & 0x20) { glyph[10] |= 0x04; glyph[11] |= 0x04; }
    if (mask & 0x40) { glyph[14] |= 0x20; glyph[15] |= 0x20; }
    if (mask & 0x80) { glyph[14] |= 0x04; glyph[15] |= 0x04; }
}

static void fb_draw_char(BootInfo *info, uint32_t x, uint32_t y,
                          uint16_t c, uint32_t fg, uint32_t bg)
{
    extern bool wm_is_gui_active(void);
    extern void wm_term_set_cell(int row, int col, uint16_t c, uint32_t fg, uint32_t bg);
    extern void comp_draw_char(uint32_t x, uint32_t y, uint16_t c, uint32_t fg, uint32_t bg);
    extern uint32_t console_start_x;
    extern uint32_t console_start_y;
    if (wm_is_gui_active()) {
        int col = (int)(x - console_start_x) / CHAR_STEP;
        int row = (int)(y - console_start_y) / LINE_STEP;
        wm_term_set_cell(row, col, c, fg, bg);
        comp_draw_char(x, y, c, fg, bg);
        return;
    }
    
    uint8_t braille_buf[16];
    const uint8_t *glyph;

    if (c >= 0x2800 && c <= 0x28FF) {
        get_braille_glyph((uint8_t)(c - 0x2800), braille_buf);
        glyph = braille_buf;
    } else if (c < 256) {
        glyph = font_8x16[c];
    } else {
        glyph = font_8x16['?'];
    }

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
        /* Base interpolated color components */
        int32_t r_base = 10 + (y * 16 / h);
        int32_t g_base = 10;
        int32_t b_base = 46 + (y * 10 / h);

        uint32_t *row_ptr = (uint32_t *)(fb + y * info->fb_pitch);
        for (uint32_t x = 0; x < w; x++) {
            /* 2x2 ordered dither to prevent banding */
            int32_t dither = 0;
            uint32_t dx = x & 1;
            uint32_t dy = y & 1;
            if (dx == 0 && dy == 0) dither = -1;
            else if (dx == 1 && dy == 0) dither = 1;
            else if (dx == 0 && dy == 1) dither = 2;
            else dither = 0;

            int32_t r = r_base + dither;
            int32_t g = g_base + dither;
            int32_t b = b_base + dither;

            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;

            /* Pack as BGRA (most common UEFI framebuffer format) */
            uint32_t color = ((uint32_t)b) | ((uint32_t)g << 8) |
                             ((uint32_t)r << 16) | (0xFF000000u);
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

static bool str_starts_with(const char *str, const char *prefix)
{
    while (*prefix) {
        if (*str != *prefix) return false;
        str++;
        prefix++;
    }
    return true;
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
 * Current Working Directory & Path Resolution
 * ============================================================ */

static char current_dir[512] = "/";

/*
 * resolve_path - Resolve a user-supplied path against the cwd.
 * If 'input' starts with '/', it's already absolute.
 * Otherwise, prepend current_dir.
 */
static void resolve_path(const char *input, char *output)
{
    if (input[0] == '/') {
        str_copy(output, input);
    } else {
        str_copy(output, current_dir);
        uint32_t len = str_len(output);
        if (len > 0 && output[len - 1] != '/') {
            str_append(output, "/");
        }
        str_append(output, input);
    }
}

/*
 * path_go_up - Remove the last component from a path (cd ..)
 * "/foo/bar" -> "/foo"
 * "/foo"     -> "/"
 * "/"        -> "/"
 */
static void path_go_up(char *path)
{
    uint32_t len = str_len(path);
    if (len <= 1) return; /* Already root */

    /* Remove trailing slash if present */
    if (path[len - 1] == '/' && len > 1) {
        path[len - 1] = '\0';
        len--;
    }

    /* Find last slash */
    int last_slash = -1;
    for (uint32_t i = 0; i < len; i++) {
        if (path[i] == '/') last_slash = (int)i;
    }

    if (last_slash <= 0) {
        /* Go to root */
        path[0] = '/';
        path[1] = '\0';
    } else {
        path[last_slash] = '\0';
    }
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
uint32_t term_bg_color = 0x000F0F1A; /* Catppuccin Mocha style dark background */

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
    extern bool wm_is_gui_active(void);
    extern void wm_term_scroll(void);
    if (wm_is_gui_active()) {
        wm_term_scroll();
        return;
    }
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
    cursor_y--;
}

static void console_clear_current_line(BootInfo *info)
{
    extern bool wm_is_gui_active(void);
    extern void wm_term_clear_line(int row);
    if (wm_is_gui_active()) {
        wm_term_clear_line(cursor_y);
        cursor_x = 0;
        return;
    }
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

static int g_utf8_state = 0;
static uint32_t g_utf8_code = 0;

void console_print_char(BootInfo *info, char c, uint32_t fg, uint32_t bg)
{
    uint8_t b = (uint8_t)c;
    uint16_t decoded_c = 0;

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

    if (g_utf8_state == 0) {
        if ((b & 0x80) == 0) {
            decoded_c = b;
        } else if ((b & 0xE0) == 0xC0) {
            g_utf8_code = b & 0x1F;
            g_utf8_state = 1;
            return;
        } else if ((b & 0xF0) == 0xE0) {
            g_utf8_code = b & 0x0F;
            g_utf8_state = 2;
            return;
        } else {
            return; // ignore invalid lead bytes
        }
    } else {
        if ((b & 0xC0) == 0x80) {
            g_utf8_code = (g_utf8_code << 6) | (b & 0x3F);
            g_utf8_state--;
            if (g_utf8_state == 0) {
                decoded_c = (uint16_t)g_utf8_code;
            } else {
                return;
            }
        } else {
            g_utf8_state = 0;
            return;
        }
    }

    if (decoded_c == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (console_start_y + cursor_y * LINE_STEP >= console_end_y) {
            console_scroll(info);
        }
    } else if (decoded_c == '\r') {
        cursor_x = 0;
    } else if (decoded_c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            fb_draw_char(info, console_start_x + cursor_x * CHAR_STEP, console_start_y + cursor_y * LINE_STEP, ' ', fg, bg);
        }
    } else {
        fb_draw_char(info, console_start_x + cursor_x * CHAR_STEP, console_start_y + cursor_y * LINE_STEP, decoded_c, fg, bg);
        cursor_x++;
        if (console_start_x + cursor_x * CHAR_STEP >= console_end_x) {
            cursor_x = 0;
            cursor_y++;
            if (console_start_y + cursor_y * LINE_STEP >= console_end_y) {
                console_scroll(info);
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

#define HISTORY_MAX_ITEMS 32
#define HISTORY_ITEM_LEN  64
static char history[HISTORY_MAX_ITEMS][HISTORY_ITEM_LEN];
static int history_count = 0;
static int history_index = -1;

static int input_cursor = 0;

static bool alt_pressed = false;
static bool layout_ru = false;

static char translate_scancode_ru(uint8_t sc, bool shift)
{
    /* QWERTY to JCUKEN layout translation using CP866 character codes */
    static const uint8_t ru_lower[59] = {
        0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
        '\t', 0xA9, 0xE6, 0xE3, 0xAA, 0xA5, 0xAD, 0xA3, 0xE8, 0xE9, 0xA7, 0xE5, 0xEA, '\n',
        0,   0xE4, 0xEB, 0xA2, 0xA0, 0xAF, 0xE0, 0xAE, 0xAB, 0xA4, 0xE6, 0xED, 0,
        0,   0xEF, 0xE7, 0xE1, 0xAC, 0xA8, 0xE2, 0xEC, 0xA1, 0xEE, '.', 0, '*', 0, ' '
    };

    static const uint8_t ru_upper[59] = {
        0,   27,  '!', '"', 0xFC, ';', '%', ':', '?', '*', '(', ')', '_', '+', '\b',
        '\t', 0x89, 0x96, 0x93, 0x8A, 0x85, 0x8D, 0x83, 0x98, 0x99, 0x87, 0x95, 0x9A, '\n',
        0,   0x94, 0x9B, 0x82, 0x80, 0x8F, 0x90, 0x8E, 0x8B, 0x84, 0x96, 0x9D, 0,
        0,   0x9F, 0x97, 0x91, 0x8C, 0x88, 0x92, 0x8C, 0x81, 0x9E, ',', 0, '*', 0, ' '
    };

    if (sc >= 59) return 0;
    return shift ? (char)ru_upper[sc] : (char)ru_lower[sc];
}

static void load_history(void)
{
    VfsFile *f = vfs_open("/history.txt");
    if (!f) return;

    static char file_buf[2048];
    int bytes = vfs_read(f, file_buf, sizeof(file_buf) - 1);
    vfs_close(f);

    if (bytes <= 0) return;
    file_buf[bytes] = '\0';

    history_count = 0;
    const char *p = file_buf;
    while (*p) {
        char line[HISTORY_ITEM_LEN];
        int li = 0;
        while (*p && *p != '\n' && *p != '\r' && li < (HISTORY_ITEM_LEN - 1)) {
            line[li++] = *p++;
        }
        line[li] = '\0';

        while (*p == '\n' || *p == '\r') p++;

        if (li > 0 && history_count < HISTORY_MAX_ITEMS) {
            str_copy(history[history_count++], line);
        }
    }
    history_index = history_count;
}

static void add_history(const char *cmd)
{
    if (str_len(cmd) == 0) return;

    if (history_count > 0 && str_compare(history[history_count - 1], cmd) == 0) {
        history_index = history_count;
        return;
    }

    VfsFile *f = vfs_open_flags("/history.txt", VFS_O_WRITE | VFS_O_CREATE | VFS_O_APPEND);
    if (f) {
        vfs_write(f, cmd, str_len(cmd));
        vfs_write(f, "\n", 1);
        vfs_close(f);
    }

    if (history_count >= HISTORY_MAX_ITEMS) {
        for (int i = 1; i < HISTORY_MAX_ITEMS; i++) {
            str_copy(history[i - 1], history[i]);
        }
        str_copy(history[HISTORY_MAX_ITEMS - 1], cmd);
    } else {
        str_copy(history[history_count++], cmd);
    }
    history_index = history_count;
}

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
    const char* commands[] = {"help", "clear", "about", "mmap", "neofetch", "tasks", "reboot", "poweroff", "panic", "heap_test", "ifconfig", "dhcp", "ping", "dns", "wget", "wynpkg", "wlang", "run", "wynasm", "wynrun"};
    int cmd_count = 20;
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

static void draw_input_line(BootInfo *info, const char *prompt, const char *input, int input_cursor_offset, const char *suggestion)
{
    mouse_hide();
    disable_serial_mirror = true;

    /* Clear line, then print prompt + user text */
    console_clear_current_line(info);
    console_print_string(info, prompt, 0x00886EFF, term_bg_color);

    uint32_t prompt_cur_x = cursor_x;
    uint32_t prompt_cur_y = cursor_y;

    console_print_string(info, input, 0x00FFFFFF, term_bg_color);

    /* Draw inline autocomplete suggestion in dim color */
    if (suggestion && suggestion[0] != '\0') {
        int input_len = str_len(input);
        const char *suffix = suggestion + input_len;
        console_print_string(info, suffix, 0x00555577, term_bg_color);
    }

    /* Reset cursor back to specific input cursor offset and render cursor block */
    cursor_x = prompt_cur_x + input_cursor_offset;
    cursor_y = prompt_cur_y;
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

BootInfo *g_ls_info = NULL;
uint32_t g_ls_bg_color = 0;

void ls_callback(VfsNode *node) {
    char size_buf[32];
    uint_to_str(node->size, size_buf);
    
    if (node->is_dir) {
        console_print_string(g_ls_info, "  [DIR]  ", 0x00FFFF00, g_ls_bg_color);
    } else {
        console_print_string(g_ls_info, "  [FILE] ", 0x0000FF00, g_ls_bg_color);
    }
    
    console_print_string(g_ls_info, node->name, 0x00FFFFFF, g_ls_bg_color);
    
    if (!node->is_dir) {
        console_print_string(g_ls_info, " (", 0x00CCCCCC, g_ls_bg_color);
        console_print_string(g_ls_info, size_buf, 0x00CCCCCC, g_ls_bg_color);
        console_print_string(g_ls_info, " bytes)", 0x00CCCCCC, g_ls_bg_color);
    }
    console_print_string(g_ls_info, "\n", 0, g_ls_bg_color);
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

/* WynLang output callback — called by the interpreter's print() */
static BootInfo *g_wlang_info = NULL;
void wlang_print_output(const char *text)
{
    if (g_wlang_info) {
        console_print_string(g_wlang_info, text, 0x00E0E0E0, term_bg_color);
    }
}

void wlang_read_input(char *buf, uint32_t max_len)
{
    if (!g_wlang_info) {
        if (max_len > 0) buf[0] = '\0';
        return;
    }
    int lpos = 0;
    memset(buf, 0, max_len);
    bool repl_shift = false;

    while (1) {
        extern bool keyboard_has_scancode(void);
        extern uint8_t keyboard_pop_scancode(void);

        if (!keyboard_has_scancode()) {
            /* Also check serial */
            uint8_t lsr = 0;
            __asm__ volatile("inb %1, %0" : "=a"(lsr) : "Nd"((uint16_t)0x3FD));
            if (lsr & 0x01) {
                uint8_t ch = 0;
                __asm__ volatile("inb %1, %0" : "=a"(ch) : "Nd"((uint16_t)0x3F8));
                if (ch == '\r' || ch == '\n') break;
                if (ch == 0x7F || ch == '\b') {
                    if (lpos > 0) { lpos--; buf[lpos] = '\0'; console_print_char(g_wlang_info, '\b', 0, term_bg_color); }
                    continue;
                }
                if (lpos < (int)(max_len - 1)) {
                    buf[lpos++] = (char)ch;
                    buf[lpos] = '\0';
                    console_print_char(g_wlang_info, (char)ch, 0x00FFFFFF, term_bg_color);
                }
            }
            __asm__ volatile("hlt");
            continue;
        }

        uint8_t sc = keyboard_pop_scancode();
        if (sc & 0x80) { /* Key release */
            uint8_t released_sc = sc & 0x7F;
            if (released_sc == 0x2A || released_sc == 0x36) {
                repl_shift = false;
            }
            continue;
        }
        if (sc == 0x2A || sc == 0x36) { /* Shift press */
            repl_shift = true;
            continue;
        }

        if (sc == 0x1C) { /* Enter */
            console_print_string(g_wlang_info, "\n", 0, term_bg_color);
            break;
        }
        if (sc == 0x0E) { /* Backspace */
            if (lpos > 0) {
                lpos--;
                buf[lpos] = '\0';
                console_print_char(g_wlang_info, '\b', 0, term_bg_color);
            }
            continue;
        }

        char ch = 0;
        if (sc < 59) {
            ch = repl_shift ? scancode_to_ascii_upper[sc] : scancode_to_ascii_lower[sc];
        }
        if (ch && lpos < (int)(max_len - 1)) {
            buf[lpos++] = ch;
            buf[lpos] = '\0';
            console_print_char(g_wlang_info, ch, 0x00FFFFFF, term_bg_color);
        }
    }
}

static void execute_command(BootInfo *info, const char *cmd)
{
    if (str_compare(cmd, "help") == 0) {
        console_print_string(info, "Available commands:\n", 0x0000FF00, term_bg_color);
        console_print_string(info, "\n  Filesystem:\n", 0x0000FFFF, term_bg_color);
        console_print_string(info, "  ls [path]         - List directory contents\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  cat <file>        - Print file contents\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  touch <file>      - Create an empty file\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  mkdir <dir>       - Create a directory\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  rm <path>         - Delete a file or empty directory\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  echo <text> > <f> - Write text to a file\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  cd <dir>          - Change current directory\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  pwd               - Print current directory\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  stat <path>       - Show file/directory info\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n  System:\n", 0x0000FFFF, term_bg_color);
        console_print_string(info, "  help              - Show this help message\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  clear             - Clear the terminal\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  about             - System information\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  mmap              - Print memory map\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  neofetch          - OS logo and specs\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  tasks             - Active system tasks\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  reboot / poweroff - Reboot or shut down\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n  Debug:\n", 0x0000FFFF, term_bg_color);
        console_print_string(info, "  panic             - Trigger CPU exception\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  heap_test         - Memory allocation test\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  sched_test        - Multitasking test\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  disk_dump         - FAT32 disk debug info\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n  Network:\n", 0x0000FFFF, term_bg_color);
        console_print_string(info, "  ifconfig          - Show network configuration\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  dhcp              - Request IP via DHCP\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  ping <ip>         - Send ICMP echo request\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  dns <hostname>    - Resolve hostname to IP\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  wget <url>        - HTTP GET request\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n  Package Manager:\n", 0x0000FFFF, term_bg_color);
        console_print_string(info, "  wynpkg <cmd>      - Package manager commands\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n  WynLang:\n", 0x0000FFFF, term_bg_color);
        console_print_string(info, "  wlang             - WynLang REPL (interactive)\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  run <file.wyn>    - Run a WynLang script\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "  wlang -e <code>   - Execute one-liner\n", 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n  Desktop:\n", 0x0000FFFF, term_bg_color);
        console_print_string(info, "  gui               - Launch WynlandDE desktop\n", 0x00FFFFFF, term_bg_color);
    } else if (str_compare(cmd, "pwd") == 0) {
        console_print_string(info, current_dir, 0x0000FFFF, term_bg_color);
        console_print_string(info, "\n", 0, term_bg_color);

    } else if (str_starts_with(cmd, "cd ")) {
        const char *arg = cmd + 3;
        if (str_compare(arg, "..") == 0) {
            path_go_up(current_dir);
        } else if (str_compare(arg, "/") == 0) {
            str_copy(current_dir, "/");
        } else {
            char resolved[512];
            resolve_path(arg, resolved);
            VfsStat st;
            if (vfs_stat(resolved, &st) && st.is_dir) {
                str_copy(current_dir, resolved);
            } else {
                console_print_string(info, "cd: not a directory: ", 0x00FF0000, term_bg_color);
                console_print_string(info, arg, 0x00FFFFFF, term_bg_color);
                console_print_string(info, "\n", 0, term_bg_color);
            }
        }

    } else if (str_compare(cmd, "ls") == 0 || str_starts_with(cmd, "ls ")) {
        char resolved[512];
        if (str_starts_with(cmd, "ls ")) {
            resolve_path(cmd + 3, resolved);
        } else {
            str_copy(resolved, current_dir);
        }

        extern void ls_callback(VfsNode *node);
        extern BootInfo *g_ls_info;
        extern uint32_t g_ls_bg_color;

        g_ls_info = info;
        g_ls_bg_color = term_bg_color;
        console_print_string(info, "Directory listing of ", 0x0000FFFF, term_bg_color);
        console_print_string(info, resolved, 0x00FFFFFF, term_bg_color);
        console_print_string(info, ":\n", 0x0000FFFF, term_bg_color);

        if (!vfs_readdir(resolved, ls_callback)) {
            console_print_string(info, "Error: Failed to read directory or directory not found!\n", 0x00FF0000, term_bg_color);
        }

    } else if (str_starts_with(cmd, "cat ")) {
        char resolved[512];
        resolve_path(cmd + 4, resolved);
        VfsFile *file = vfs_open(resolved);

        if (!file) {
            console_print_string(info, "Error: Cannot open file: ", 0x00FF0000, term_bg_color);
            console_print_string(info, cmd + 4, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);
        } else {
            char read_buf[512];
            int bytes_read = 0;
            while ((bytes_read = vfs_read(file, read_buf, 511)) > 0) {
                read_buf[bytes_read] = '\0';
                console_print_string(info, read_buf, 0x00E0E0E0, term_bg_color);
            }
            vfs_close(file);
            console_print_string(info, "\n", 0, term_bg_color);
        }

    } else if (str_starts_with(cmd, "touch ")) {
        char resolved[512];
        resolve_path(cmd + 6, resolved);
        if (vfs_create(resolved)) {
            console_print_string(info, "Created: ", 0x0000FF00, term_bg_color);
            console_print_string(info, resolved, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);
        } else {
            console_print_string(info, "Error: Failed to create file: ", 0x00FF0000, term_bg_color);
            console_print_string(info, cmd + 6, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);
        }

    } else if (str_starts_with(cmd, "mkdir ")) {
        char resolved[512];
        resolve_path(cmd + 6, resolved);
        if (vfs_mkdir(resolved)) {
            console_print_string(info, "Created directory: ", 0x0000FF00, term_bg_color);
            console_print_string(info, resolved, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);
        } else {
            console_print_string(info, "Error: Failed to create directory: ", 0x00FF0000, term_bg_color);
            console_print_string(info, cmd + 6, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);
        }

    } else if (str_starts_with(cmd, "rm ")) {
        char resolved[512];
        resolve_path(cmd + 3, resolved);
        if (vfs_delete(resolved)) {
            console_print_string(info, "Deleted: ", 0x0000FF00, term_bg_color);
            console_print_string(info, resolved, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);
        } else {
            console_print_string(info, "Error: Failed to delete: ", 0x00FF0000, term_bg_color);
            console_print_string(info, cmd + 3, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);
        }

    } else if (str_starts_with(cmd, "echo ")) {
        /* Parse: echo <text> > <filename> */
        const char *rest = cmd + 5;
        const char *redir = rest;
        const char *redir_pos = NULL;

        /* Find " > " redirect operator */
        while (*redir) {
            if (*redir == '>' && redir > rest && *(redir - 1) == ' ') {
                redir_pos = redir;
                break;
            }
            redir++;
        }

        if (redir_pos && *(redir_pos + 1) == ' ') {
            /* Extract text (before " > ") */
            char text[512];
            uint32_t text_len = (uint32_t)(redir_pos - 1 - rest);
            for (uint32_t i = 0; i < text_len; i++) text[i] = rest[i];
            text[text_len] = '\n';
            text[text_len + 1] = '\0';

            /* Extract filename (after "> ") */
            const char *filename = redir_pos + 2;
            char resolved[512];
            resolve_path(filename, resolved);

            VfsFile *file = vfs_open_flags(resolved, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
            if (file) {
                uint32_t tlen = 0;
                while (text[tlen]) tlen++;
                int written = vfs_write(file, text, tlen);
                vfs_close(file);
                if (written > 0) {
                    console_print_string(info, "Wrote ", 0x0000FF00, term_bg_color);
                    char buf[16];
                    uint_to_str((uint64_t)written, buf);
                    console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
                    console_print_string(info, " bytes to ", 0x0000FF00, term_bg_color);
                    console_print_string(info, resolved, 0x00FFFFFF, term_bg_color);
                    console_print_string(info, "\n", 0, term_bg_color);
                } else {
                    console_print_string(info, "Error: Write failed!\n", 0x00FF0000, term_bg_color);
                }
            } else {
                console_print_string(info, "Error: Cannot open file for writing: ", 0x00FF0000, term_bg_color);
                console_print_string(info, filename, 0x00FFFFFF, term_bg_color);
                console_print_string(info, "\n", 0, term_bg_color);
            }
        } else {
            /* No redirect, just print the text */
            console_print_string(info, rest, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);
        }

    } else if (str_starts_with(cmd, "stat ")) {
        char resolved[512];
        resolve_path(cmd + 5, resolved);
        VfsStat st;
        if (vfs_stat(resolved, &st)) {
            console_print_string(info, "  File: ", 0x00CCCCCC, term_bg_color);
            console_print_string(info, st.name, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);

            console_print_string(info, "  Type: ", 0x00CCCCCC, term_bg_color);
            console_print_string(info, st.is_dir ? "Directory" : "Regular File", 0x0000FF00, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);

            console_print_string(info, "  Size: ", 0x00CCCCCC, term_bg_color);
            char buf[32];
            uint_to_str(st.size, buf);
            console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
            console_print_string(info, " bytes\n", 0x00CCCCCC, term_bg_color);

            console_print_string(info, "  Cluster: ", 0x00CCCCCC, term_bg_color);
            uint_to_str(st.first_cluster, buf);
            console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);

            console_print_string(info, "  Attr: ", 0x00CCCCCC, term_bg_color);
            if (st.attr & 0x01) console_print_string(info, "R ", 0x00FFFF00, term_bg_color);
            if (st.attr & 0x02) console_print_string(info, "H ", 0x00FFFF00, term_bg_color);
            if (st.attr & 0x04) console_print_string(info, "S ", 0x00FFFF00, term_bg_color);
            if (st.attr & 0x10) console_print_string(info, "D ", 0x00FFFF00, term_bg_color);
            if (st.attr & 0x20) console_print_string(info, "A ", 0x00FFFF00, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);
        } else {
            console_print_string(info, "Error: Cannot stat: ", 0x00FF0000, term_bg_color);
            console_print_string(info, cmd + 5, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);
        }
    } else if (str_compare(cmd, "disk_dump") == 0) {
        extern uint32_t first_data_sector;
        extern uint32_t root_cluster;
        extern uint32_t reserved_sectors;
        extern uint32_t fat_count;
        extern uint32_t sectors_per_fat;
        
        char buf[64];
        console_print_string(info, "reserved_sectors:  ", 0x00CCCCCC, term_bg_color);
        uint_to_str(reserved_sectors, buf);
        console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n", 0, term_bg_color);
        
        console_print_string(info, "fat_count:         ", 0x00CCCCCC, term_bg_color);
        uint_to_str(fat_count, buf);
        console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n", 0, term_bg_color);

        console_print_string(info, "sectors_per_fat:   ", 0x00CCCCCC, term_bg_color);
        uint_to_str(sectors_per_fat, buf);
        console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n", 0, term_bg_color);

        console_print_string(info, "first_data_sector: ", 0x00CCCCCC, term_bg_color);
        uint_to_str(first_data_sector, buf);
        console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n", 0, term_bg_color);
        
        console_print_string(info, "root_cluster:      ", 0x00CCCCCC, term_bg_color);
        uint_to_str(root_cluster, buf);
        console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n", 0, term_bg_color);
        
        extern bool ahci_read(uint32_t lba, uint32_t count, void *buf);
        uint8_t temp[512];
        uint32_t root_sector = first_data_sector + (root_cluster - 2) * 1;
        if (ahci_read(root_sector, 1, temp)) {
            console_print_string(info, "Root sector dump (first 128 bytes):\n", 0x00FFFF00, term_bg_color);
            const char *hex_chars = "0123456789ABCDEF";
            for (int r = 0; r < 4; r++) {
                for (int c = 0; c < 32; c++) {
                    uint8_t val = temp[r * 32 + c];
                    char hex_buf[4];
                    hex_buf[0] = hex_chars[(val >> 4) & 0x0F];
                    hex_buf[1] = hex_chars[val & 0x0F];
                    hex_buf[2] = ' ';
                    hex_buf[3] = '\0';
                    console_print_string(info, hex_buf, 0x00FFFFFF, term_bg_color);
                }
                console_print_string(info, "\n", 0, term_bg_color);
            }
        } else {
            console_print_string(info, "Error: Failed to read root sector!\n", 0x00FF0000, term_bg_color);
        }
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
    } else if (str_compare(cmd, "gui") == 0) {
        console_print_string(info, "Launching WynlandDE Desktop...\n", 0x0000FF00, term_bg_color);
        for (volatile int i = 0; i < 10000000; i++); /* Brief pause */

        extern void wm_init(void);
        wm_init();

        cursor_x = 0;
        cursor_y = 0;

        console_print_string(info, "Welcome to WynlandDE!\n", 0x0088C0D0, term_bg_color);
        console_print_string(info, "Desktop with Nord theme active.\n\n", 0x00ECEFF4, term_bg_color);
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
        char buf[64];
        uint32_t c_logo  = 0x0088C0D0; // Nord8 Cyan
        uint32_t c_label = 0x0081A1C1; // Nord9 Blue
        uint32_t c_val   = 0x00ECEFF4; // Nord4 White
        uint32_t c_user  = 0x00A3BE8C; // Nord14 Green

        /* Line 1: User & Host */
        console_print_string(info, "\xE2\xA0\x80\xE2\xA0\x80\xE2\xA1\x94\xE2\xA0\x89\xE2\xA0\x91\xE2\xA2\xA4\xE2\xA1\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA2\x80\xE2\xA3\xB0\xE2\xA0\x8B\xE2\xA0\x89\xE2\xA0\x89\xE2\xA0\x93\xE2\xA1\x86\xE2\xA0\x80\xE2\xA0\x80", c_logo, term_bg_color);
        console_print_string(info, "  ", 0, term_bg_color);
        console_print_string(info, "wynland", c_user, term_bg_color);
        console_print_string(info, "@", c_val, term_bg_color);
        console_print_string(info, "wynlandos\n", c_user, term_bg_color);

        /* Line 2: Separator */
        console_print_string(info, "\xE2\xA0\x80\xE2\xA3\xB8\xE2\xA0\x81\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x99\xE2\xA2\xA6\xE2\xA1\x80\xE2\xA2\xB8\xE2\xA1\x89\xE2\xA0\x93\xE2\xA0\xB2\xE2\xA3\x84\xE2\xA1\x80\xE2\xA2\x80\xE2\xA1\x9E\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA3\x80\xE2\xA3\xBD\xE2\xA1\xA4\xE2\xA0\x80", c_logo, term_bg_color);
        console_print_string(info, "  ", 0, term_bg_color);
        console_print_string(info, "-----------------\n", 0x004C566A, term_bg_color); // Nord3 Gray

        /* Line 3: OS */
        console_print_string(info, "\xE2\xA0\x80\xE2\xA1\x87\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x99\xE2\xA3\xA6\xE2\xA0\xB7\xE2\xA0\x84\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x99\xE2\xA0\x9E\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x92\xE2\xA0\x9A\xE2\xA1\x8F\xE2\xA0\x81", c_logo, term_bg_color);
        console_print_string(info, "  ", 0, term_bg_color);
        console_print_string(info, "OS: ", c_label, term_bg_color);
        console_print_string(info, "WynlandOS v0.1 x86_64\n", c_val, term_bg_color);

        /* Line 4: Host */
        console_print_string(info, "\xE2\xA0\x80\xE2\xA1\x87\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA3\x80\xE2\xA3\x80\xE2\xA1\x9A\xE2\xA0\x93\xE2\xA0\x92\xE2\xA0\x92\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA1\x87\xE2\xA0\x80", c_logo, term_bg_color);
        console_print_string(info, "  ", 0, term_bg_color);
        console_print_string(info, "Host: ", c_label, term_bg_color);
        console_print_string(info, "QEMU Virtual Machine\n", c_val, term_bg_color);

        /* Line 5: Kernel */
        console_print_string(info, "\xE2\xA0\x80\xE2\xA1\x87\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA1\x8E\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x88\xE2\xA1\x86\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x98\xE2\xA2\x84\xE2\xA3\x80\xE2\xA3\x80\xE2\xA1\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA3\xB8\xE2\xA0\x81\xE2\xA0\x80", c_logo, term_bg_color);
        console_print_string(info, "  ", 0, term_bg_color);
        console_print_string(info, "Kernel: ", c_label, term_bg_color);
        console_print_string(info, "Freestanding Monolithic (C11)\n", c_val, term_bg_color);

        /* Line 6: Uptime */
        console_print_string(info, "\xE2\xA0\x88\xE2\xA3\x87\xE2\xA0\x80\xE2\xA0\x90\xE2\xA1\xB6\xE2\xA0\x96\xE2\xA3\xB2\xE2\xA3\xB6\xE2\xA1\x86\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA3\xB6\xE2\xA3\xB6\xE2\xA1\x92\xE2\xA0\xB2\xE2\xA1\x92\xE2\xA0\x80\xE2\xA2\x80\xE2\xA1\xBC\xE2\xA0\x81\xE2\xA0\x80\xE2\xA0\x80", c_logo, term_bg_color);
        console_print_string(info, "  ", 0, term_bg_color);
        console_print_string(info, "Uptime: ", c_label, term_bg_color);
        extern uint64_t timer_get_ticks(void);
        uint64_t total_sec = timer_get_ticks() / 100;
        uint64_t hrs = total_sec / 3600;
        uint64_t mins = (total_sec / 60) % 60;
        uint64_t secs = total_sec % 60;
        if (hrs > 0) {
            uint_to_str(hrs, buf); console_print_string(info, buf, c_val, term_bg_color);
            console_print_string(info, " hours, ", c_val, term_bg_color);
            uint_to_str(mins, buf); console_print_string(info, buf, c_val, term_bg_color);
            console_print_string(info, " mins", c_val, term_bg_color);
        } else if (mins > 0) {
            uint_to_str(mins, buf); console_print_string(info, buf, c_val, term_bg_color);
            console_print_string(info, " mins, ", c_val, term_bg_color);
            uint_to_str(secs, buf); console_print_string(info, buf, c_val, term_bg_color);
            console_print_string(info, " secs", c_val, term_bg_color);
        } else {
            uint_to_str(secs, buf); console_print_string(info, buf, c_val, term_bg_color);
            console_print_string(info, " secs", c_val, term_bg_color);
        }
        console_print_string(info, "\n", c_val, term_bg_color);

        /* Line 7: Shell */
        console_print_string(info, "\xE2\xA0\xB0\xE2\xA3\x96\xE2\xA0\xBA\xE2\xA0\xA7\xE2\xA2\xB8\xE2\xA0\x81\xE2\xA0\x80\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA0\x87\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA0\x87\xE2\xA0\x80\xE2\xA1\x87\xE2\xA0\x80\xE2\xA0\x89\xE2\xA3\xA9\xE2\xA0\x87\xE2\xA0\x80\xE2\xA0\x80", c_logo, term_bg_color);
        console_print_string(info, "  ", 0, term_bg_color);
        console_print_string(info, "Shell: ", c_label, term_bg_color);
        console_print_string(info, "WynlandShell v0.1\n", c_val, term_bg_color);

        /* Line 8: Resolution */
        console_print_string(info, "\xE2\xA0\x80\xE2\xA0\x88\xE2\xA3\xB3\xE2\xA0\x80\xE2\xA3\xA8\xE2\xA2\x83\xE2\xA0\x80\xE2\xA0\x88\xE2\xA0\x89\xE2\xA0\x80\xE2\xA0\x92\xE2\xA0\x82\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x88\xE2\xA0\x81\xE2\xA2\x80\xE2\xA0\x84\xE2\xA1\xA1\xE2\xA0\x80\xE2\xA2\xBC\xE2\xA1\x81\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80", c_logo, term_bg_color);
        console_print_string(info, "  ", 0, term_bg_color);
        console_print_string(info, "Resolution: ", c_label, term_bg_color);
        uint_to_str(info->fb_width, buf); console_print_string(info, buf, c_val, term_bg_color);
        console_print_string(info, "x", c_val, term_bg_color);
        uint_to_str(info->fb_height, buf); console_print_string(info, buf, c_val, term_bg_color);
        console_print_string(info, "\n", c_val, term_bg_color);

        /* Line 9: DE */
        console_print_string(info, "\xE2\xA0\x80\xE2\xA2\xB0\xE2\xA3\x83\xE2\xA3\x88\xE2\xA3\x80\xE2\xA0\x81\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\xA6\xE2\xA0\x94\xE2\xA0\x93\xE2\xA0\xB2\xE2\xA1\xB2\xE2\xA0\x83\xE2\xA0\x80\xE2\xA0\x80\xE2\xA2\x88\xE2\xA3\x80\xE2\xA3\x80\xE2\xA3\x80\xE2\xA3\xB9\xE2\xA1\x84\xE2\xA0\x80\xE2\xA0\x80", c_logo, term_bg_color);
        console_print_string(info, "  ", 0, term_bg_color);
        console_print_string(info, "DE: ", c_label, term_bg_color);
        console_print_string(info, "WynlandDE\n", c_val, term_bg_color);

        /* Line 10: WM */
        console_print_string(info, "\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x89\xE2\xA2\xB3\xE2\xA0\xB2\xE2\xA0\xA4\xE2\xA2\x84\xE2\xA3\x80\xE2\xA3\x80\xE2\xA3\x80\xE2\xA3\xA0\xE2\xA2\xB6\xE2\xA0\x92\xE2\xA3\x8F\xE2\xA0\x89\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80", c_logo, term_bg_color);
        console_print_string(info, "  ", 0, term_bg_color);
        console_print_string(info, "WM: ", c_label, term_bg_color);
        console_print_string(info, "Stacking Window Manager (Nord Theme)\n", c_val, term_bg_color);

        /* Line 11: Terminal */
        console_print_string(info, "\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x88\xE2\xA3\xB3\xE2\xA0\x92\xE2\xA2\xB2\xE2\xA0\x8B\xE2\xA2\xB1\xE2\xA0\xA4\xE2\xA0\xA7\xE2\xA0\x9A\xE2\xA0\x8B\xE2\xA0\x98\xE2\xA1\x86\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80", c_logo, term_bg_color);
        console_print_string(info, "  ", 0, term_bg_color);
        console_print_string(info, "Terminal: ", c_label, term_bg_color);
        console_print_string(info, "WynlandOS Terminal\n", c_val, term_bg_color);

        /* Line 12: CPU */
        console_print_string(info, "\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x98\xE2\xA0\x93\xE2\xA1\x86\xE2\xA0\x88\xE2\xA0\x92\xE2\xA0\x81\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA2\xB9\xE2\xA1\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80", c_logo, term_bg_color);
        console_print_string(info, "  ", 0, term_bg_color);
        console_print_string(info, "CPU: ", c_label, term_bg_color);
        console_print_string(info, "QEMU x86_64 Virtual CPU\n", c_val, term_bg_color);

        /* Line 13: GPU */
        console_print_string(info, "\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA3\xBC\xE2\xA3\x81\xE2\xA3\x80\xE2\xA3\x80\xE2\xA3\x80\xE2\xA3\x80\xE2\xA3\x80\xE2\xA3\x80\xE2\xA3\x80\xE2\xA3\x80\xE2\xA3\x80\xE2\xA1\x87\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80", c_logo, term_bg_color);
        console_print_string(info, "  ", 0, term_bg_color);
        console_print_string(info, "GPU: ", c_label, term_bg_color);
        console_print_string(info, "VirtIO-GPU Device\n", c_val, term_bg_color);

        /* Line 14: Memory */
        console_print_string(info, "                            ", c_logo, term_bg_color);
        console_print_string(info, "Memory: ", c_label, term_bg_color);
        extern uint64_t pmm_get_used_memory(void);
        extern uint64_t pmm_get_total_memory(void);
        uint_to_str(pmm_get_used_memory() / (1024 * 1024), buf); console_print_string(info, buf, c_val, term_bg_color);
        console_print_string(info, " MB / ", c_val, term_bg_color);
        uint_to_str(pmm_get_total_memory() / (1024 * 1024), buf); console_print_string(info, buf, c_val, term_bg_color);
        console_print_string(info, " MB\n", c_val, term_bg_color);

        /* Line 15: Color Blocks */
        console_print_string(info, "                            ", c_logo, term_bg_color);
        uint32_t colors[8] = {
            0x00BF616A, // Nord11 Red
            0x00A3BE8C, // Nord14 Green
            0x00EBCB8B, // Nord13 Yellow
            0x0081A1C1, // Nord9 Blue
            0x00B48EAD, // Nord15 Purple
            0x0088C0D0, // Nord8 Cyan
            0x00E5E9F0, // Nord5 Light Gray
            0x00ECEFF4  // Nord4 White
        };
        char block_str[4] = {(char)219, (char)219, ' ', '\0'};
        for (int i = 0; i < 8; i++) {
            console_print_string(info, block_str, colors[i], term_bg_color);
        }
        console_print_string(info, "\n", c_val, term_bg_color);
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

    /* ---- Network Commands ---- */
    } else if (str_compare(cmd, "ifconfig") == 0) {
        console_print_string(info, "Network Configuration:\n", 0x0000FFFF, term_bg_color);
        console_print_string(info, "======================\n", 0x00888888, term_bg_color);

        /* MAC Address */
        uint8_t mac[6];
        net_get_mac(mac);
        const char *hex_chars = "0123456789ABCDEF";
        char mac_str[18]; /* XX:XX:XX:XX:XX:XX\0 */
        for (int i = 0; i < 6; i++) {
            mac_str[i * 3]     = hex_chars[(mac[i] >> 4) & 0x0F];
            mac_str[i * 3 + 1] = hex_chars[mac[i] & 0x0F];
            mac_str[i * 3 + 2] = (i < 5) ? ':' : '\0';
        }
        console_print_string(info, "  MAC Address: ", 0x00CCCCCC, term_bg_color);
        console_print_string(info, mac_str, 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n", 0, term_bg_color);

        /* IP Address */
        char ip_str[20];
        uint32_t ip = net_get_ip();
        net_ip_to_str(ip, ip_str);
        console_print_string(info, "  IP Address:  ", 0x00CCCCCC, term_bg_color);
        console_print_string(info, ip_str, 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n", 0, term_bg_color);

        /* Netmask */
        uint32_t mask = net_get_netmask();
        net_ip_to_str(mask, ip_str);
        console_print_string(info, "  Netmask:     ", 0x00CCCCCC, term_bg_color);
        console_print_string(info, ip_str, 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n", 0, term_bg_color);

        /* Gateway */
        uint32_t gw = net_get_gateway();
        net_ip_to_str(gw, ip_str);
        console_print_string(info, "  Gateway:     ", 0x00CCCCCC, term_bg_color);
        console_print_string(info, ip_str, 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n", 0, term_bg_color);

        /* DNS */
        uint32_t dns = net_get_dns();
        net_ip_to_str(dns, ip_str);
        console_print_string(info, "  DNS Server:  ", 0x00CCCCCC, term_bg_color);
        console_print_string(info, ip_str, 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n", 0, term_bg_color);

        /* Status */
        bool net_up = net_is_up();
        console_print_string(info, "  Status:      ", 0x00CCCCCC, term_bg_color);
        if (net_up) {
            console_print_string(info, "UP\n", 0x0000FF00, term_bg_color);
        } else {
            console_print_string(info, "DOWN\n", 0x00FF0000, term_bg_color);
        }

    } else if (str_compare(cmd, "dhcp") == 0) {
        console_print_string(info, "Requesting IP address via DHCP...\n", 0x00FFFF00, term_bg_color);
        bool ok = net_dhcp_request();
        if (ok) {
            char ip_str[20];
            uint32_t ip = net_get_ip();
            net_ip_to_str(ip, ip_str);
            console_print_string(info, "DHCP success! Obtained IP: ", 0x0000FF00, term_bg_color);
            console_print_string(info, ip_str, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);
        } else {
            console_print_string(info, "DHCP request failed.\n", 0x00FF0000, term_bg_color);
        }

    } else if (str_starts_with(cmd, "ping ")) {
        const char *arg = cmd + 5;
        uint32_t target_ip = net_str_to_ip(arg);
        if (target_ip == 0) {
            console_print_string(info, "Error: Invalid IP address: ", 0x00FF0000, term_bg_color);
            console_print_string(info, arg, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);
        } else {
            console_print_string(info, "PING ", 0x00FFFF00, term_bg_color);
            console_print_string(info, arg, 0x00FFFFFF, term_bg_color);
            console_print_string(info, " ...\n", 0x00FFFF00, term_bg_color);
            int result = net_ping(target_ip);
            if (result >= 0) {
                char buf[32];
                console_print_string(info, "Reply received: ", 0x0000FF00, term_bg_color);
                uint_to_str((uint64_t)result, buf);
                console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
                console_print_string(info, " poll cycles\n", 0x00CCCCCC, term_bg_color);
            } else {
                console_print_string(info, "Request timed out.\n", 0x00FF0000, term_bg_color);
            }
        }

    } else if (str_starts_with(cmd, "dns ")) {
        const char *hostname = cmd + 4;
        console_print_string(info, "Resolving ", 0x00FFFF00, term_bg_color);
        console_print_string(info, hostname, 0x00FFFFFF, term_bg_color);
        console_print_string(info, " ...\n", 0x00FFFF00, term_bg_color);
        uint32_t resolved_ip = 0;
        if (net_dns_resolve(hostname, &resolved_ip)) {
            char ip_str[20];
            net_ip_to_str(resolved_ip, ip_str);
            console_print_string(info, "Resolved to: ", 0x0000FF00, term_bg_color);
            console_print_string(info, ip_str, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);
        } else {
            console_print_string(info, "DNS resolution failed for: ", 0x00FF0000, term_bg_color);
            console_print_string(info, hostname, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);
        }

    } else if (str_starts_with(cmd, "wget ")) {
        const char *url = cmd + 5;
        char hostname[128];
        char path[256];
        int hi = 0;

        /* Extract hostname (up to first '/' or end of string) */
        while (url[hi] && url[hi] != '/' && hi < 127) {
            hostname[hi] = url[hi];
            hi++;
        }
        hostname[hi] = '\0';

        /* Extract path (default to "/" if none given) */
        if (url[hi] == '/') {
            int pi = 0;
            while (url[hi] && pi < 255) {
                path[pi++] = url[hi++];
            }
            path[pi] = '\0';
        } else {
            path[0] = '/';
            path[1] = '\0';
        }

        console_print_string(info, "GET http://", 0x00FFFF00, term_bg_color);
        console_print_string(info, hostname, 0x00FFFFFF, term_bg_color);
        console_print_string(info, path, 0x00FFFFFF, term_bg_color);
        console_print_string(info, " ...\n", 0x00FFFF00, term_bg_color);

        char resp_buf[4096];
        HttpResponse resp;
        int ret = http_get(hostname, 80, path, resp_buf, sizeof(resp_buf), &resp);

        if (ret >= 0) {
            /* Status code */
            char buf[32];
            console_print_string(info, "Status: ", 0x00CCCCCC, term_bg_color);
            uint_to_str((uint64_t)resp.status_code, buf);
            if (resp.status_code >= 200 && resp.status_code < 300) {
                console_print_string(info, buf, 0x0000FF00, term_bg_color);
            } else {
                console_print_string(info, buf, 0x00FF0000, term_bg_color);
            }
            console_print_string(info, "\n", 0, term_bg_color);

            /* Content length */
            console_print_string(info, "Content-Length: ", 0x00CCCCCC, term_bg_color);
            uint_to_str((uint64_t)resp.content_length, buf);
            console_print_string(info, buf, 0x00FFFFFF, term_bg_color);
            console_print_string(info, " bytes\n", 0x00CCCCCC, term_bg_color);

            /* Body (first 512 bytes) */
            console_print_string(info, "\n--- Body (first 512 bytes) ---\n", 0x0000FFFF, term_bg_color);
            if (resp.body_len > 0) {
                uint32_t show_len = resp.body_len > 512 ? 512 : resp.body_len;
                /* Temporarily null-terminate the portion we want to print */
                char saved = resp_buf[show_len];
                resp_buf[show_len] = '\0';
                console_print_string(info, resp_buf, 0x00E0E0E0, term_bg_color);
                resp_buf[show_len] = saved;
            }
            console_print_string(info, "\n--- End ---\n", 0x0000FFFF, term_bg_color);
        } else {
            console_print_string(info, "HTTP request failed.\n", 0x00FF0000, term_bg_color);
        }

    } else if (str_compare(cmd, "wynpkg") == 0 || str_starts_with(cmd, "wynpkg ")) {
        cmd_wynpkg(info, cmd);

    } else if (str_starts_with(cmd, "wynasm ")) {
        const char *p = cmd + 7;
        while (*p && (*p == ' ' || *p == '\t')) p++;
        if (*p == '\0') {
            console_print_string(info, "Usage: wynasm <src.wasm> <dst.wbin>\n", 0x00FF0000, term_bg_color);
        } else {
            char src_arg[256];
            uint32_t i = 0;
            while (*p && *p != ' ' && *p != '\t' && i < 255) {
                src_arg[i++] = *p++;
            }
            src_arg[i] = '\0';

            while (*p && (*p == ' ' || *p == '\t')) p++;
            if (*p == '\0') {
                console_print_string(info, "Usage: wynasm <src.wasm> <dst.wbin>\n", 0x00FF0000, term_bg_color);
            } else {
                char dst_arg[256];
                uint32_t j = 0;
                while (*p && *p != ' ' && *p != '\t' && j < 255) {
                    dst_arg[j++] = *p++;
                }
                dst_arg[j] = '\0';

                char resolved_src[512];
                char resolved_dst[512];
                resolve_path(src_arg, resolved_src);
                resolve_path(dst_arg, resolved_dst);

                console_print_string(info, "Assembling: ", 0x00FFFF00, term_bg_color);
                console_print_string(info, resolved_src, 0x00FFFFFF, term_bg_color);
                console_print_string(info, " -> ", 0x00FFFF00, term_bg_color);
                console_print_string(info, resolved_dst, 0x00FFFFFF, term_bg_color);
                console_print_string(info, "\n", 0, term_bg_color);

                extern bool wynvm_assemble(const char *src_path, const char *dest_path);
                if (wynvm_assemble(resolved_src, resolved_dst)) {
                    console_print_string(info, "Assembly completed successfully.\n", 0x0000FF00, term_bg_color);
                } else {
                    console_print_string(info, "Assembly failed.\n", 0x00FF0000, term_bg_color);
                }
            }
        }

    } else if (str_starts_with(cmd, "wynrun ")) {
        const char *p = cmd + 7;
        while (*p && (*p == ' ' || *p == '\t')) p++;
        if (*p == '\0') {
            console_print_string(info, "Usage: wynrun <file.wbin>\n", 0x00FF0000, term_bg_color);
        } else {
            char arg[256];
            uint32_t i = 0;
            while (*p && *p != ' ' && *p != '\t' && i < 255) {
                arg[i++] = *p++;
            }
            arg[i] = '\0';

            char resolved[512];
            resolve_path(arg, resolved);

            console_print_string(info, "Running VM binary: ", 0x00FFFF00, term_bg_color);
            console_print_string(info, resolved, 0x00FFFFFF, term_bg_color);
            console_print_string(info, "\n", 0, term_bg_color);

            extern bool wynvm_run(const char *bin_path);
            if (wynvm_run(resolved)) {
                console_print_string(info, "VM started successfully in background thread.\n", 0x0000FF00, term_bg_color);
            } else {
                console_print_string(info, "Failed to start VM.\n", 0x00FF0000, term_bg_color);
            }
        }

    } else if (str_compare(cmd, "wlang") == 0) {
        /* WynLang REPL mode */
        g_wlang_info = info;
        console_print_string(info, "WynLang v1.0 — Interactive Mode\n", 0x0000FFFF, term_bg_color);
        console_print_string(info, "Type code and press Enter. Type exit() to quit.\n", 0x00CCCCCC, term_bg_color);
        console_print_string(info, "Tip: use 'run <file.wyn>' for multi-line scripts.\n\n", 0x00CCCCCC, term_bg_color);

        /* Simple REPL: collect lines, execute when 'end' or single-line stmt */
        char repl_buf[2048];
        char line_buf[256];
        repl_buf[0] = '\0';
        bool in_block = false;
        bool repl_running = true;

        while (repl_running) {
            /* Print prompt */
            if (in_block) {
                console_print_string(info, "... ", 0x00888888, term_bg_color);
            } else {
                console_print_string(info, ">>> ", 0x0000FF00, term_bg_color);
            }

            /* Read a line (reusing scancode reading) */
            int lpos = 0;
            memset(line_buf, 0, sizeof(line_buf));
            bool repl_shift = false;

            while (1) {
                /* Poll keyboard */
                extern bool keyboard_has_scancode(void);
                extern uint8_t keyboard_pop_scancode(void);

                if (!keyboard_has_scancode()) {
                    /* Also check serial */
                    uint8_t lsr = 0;
                    __asm__ volatile("inb %1, %0" : "=a"(lsr) : "Nd"((uint16_t)0x3FD));
                    if (lsr & 0x01) {
                        uint8_t ch = 0;
                        __asm__ volatile("inb %1, %0" : "=a"(ch) : "Nd"((uint16_t)0x3F8));
                        if (ch == '\r' || ch == '\n') break;
                        if (ch == 0x7F || ch == '\b') {
                            if (lpos > 0) { lpos--; line_buf[lpos] = '\0'; console_print_char(info, '\b', 0, term_bg_color); }
                            continue;
                        }
                        if (lpos < 254) {
                            line_buf[lpos++] = (char)ch;
                            line_buf[lpos] = '\0';
                            console_print_char(info, (char)ch, 0x00FFFFFF, term_bg_color);
                        }
                    }
                    __asm__ volatile("hlt");
                    continue;
                }

                uint8_t sc = keyboard_pop_scancode();
                if (sc & 0x80) { /* Key release */
                    uint8_t released_sc = sc & 0x7F;
                    if (released_sc == 0x2A || released_sc == 0x36) {
                        repl_shift = false;
                    }
                    continue;
                }
                if (sc == 0x2A || sc == 0x36) { /* Shift press */
                    repl_shift = true;
                    continue;
                }

                if (sc == 0x1C) { /* Enter */
                    console_print_string(info, "\n", 0, term_bg_color);
                    break;
                }
                if (sc == 0x0E) { /* Backspace */
                    if (lpos > 0) {
                        lpos--;
                        line_buf[lpos] = '\0';
                        console_print_char(info, '\b', 0, term_bg_color);
                    }
                    continue;
                }

                char ch = 0;
                if (sc < 59) {
                    ch = repl_shift ? scancode_to_ascii_upper[sc] : scancode_to_ascii_lower[sc];
                }
                if (ch && lpos < 254) {
                    line_buf[lpos++] = ch;
                    line_buf[lpos] = '\0';
                    console_print_char(info, ch, 0x00FFFFFF, term_bg_color);
                }
            }

            /* Check for exit */
            if (str_compare(line_buf, "exit()") == 0 || str_compare(line_buf, "exit") == 0) {
                console_print_string(info, "Bye!\n", 0x0000FFFF, term_bg_color);
                repl_running = false;
                continue;
            }

            /* Check if line ends with ':' (block start) */
            uint32_t llen = str_len(line_buf);
            bool ends_colon = (llen > 0 && line_buf[llen - 1] == ':');

            /* Append to buffer */
            str_append(repl_buf, line_buf);
            str_append(repl_buf, "\n");

            if (ends_colon) {
                in_block = true;
                continue;
            }

            if (in_block) {
                /* Check if this line is 'end' */
                if (str_compare(line_buf, "end") == 0) {
                    in_block = false;
                    /* Fall through to execute */
                } else {
                    continue; /* Keep collecting block lines */
                }
            }

            /* Execute collected code */
            wlang_run(repl_buf);
            repl_buf[0] = '\0';
        }

    } else if (str_starts_with(cmd, "wlang -e ")) {
        /* Execute one-liner */
        g_wlang_info = info;
        const char *code = cmd + 9;
        wlang_run(code);

    } else if (str_starts_with(cmd, "run ")) {
        /* Run .wyn file */
        g_wlang_info = info;
        char resolved[512];
        resolve_path(cmd + 4, resolved);
        console_print_string(info, "Running: ", 0x0000FFFF, term_bg_color);
        console_print_string(info, resolved, 0x00FFFFFF, term_bg_color);
        console_print_string(info, "\n", 0, term_bg_color);
        int result = wlang_run_file(resolved);
        if (result == WLANG_OK) {
            console_print_string(info, "\n[OK]\n", 0x0000FF00, term_bg_color);
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

    /* ---- Initialize Compositor ---- */
    compositor_init(boot_info);

    /* ---- Initialize VirtIO-GPU ---- */
    extern bool virtio_gpu_init(void);
    virtio_gpu_init();

    /* ---- Initialize Serial Port (COM1) ---- */
    serial_init();
    serial_write_string("\r\nWynlandOS Kernel Starting...\r\n");

    /* ---- Initialize Filesystem ---- */
    vfs_init();
    vfs_mkdir("/apps");
    vfs_mkdir("/docs");

    /* ---- Initialize Network Stack ---- */
    net_init();
    tcp_init();

    /* ---- Draw gradient background ---- */
    serial_write_string("Main: Drawing gradient background...\r\n");
    {
        char temp_buf[32];
        serial_write_string("FB Addr: ");
        uint_to_hex(boot_info->fb_addr, temp_buf);
        serial_write_string(temp_buf);
        serial_write_string("\r\nFB Width: ");
        uint_to_str(boot_info->fb_width, temp_buf);
        serial_write_string(temp_buf);
        serial_write_string("\r\nFB Height: ");
        uint_to_str(boot_info->fb_height, temp_buf);
        serial_write_string(temp_buf);
        serial_write_string("\r\nFB Pitch: ");
        uint_to_str(boot_info->fb_pitch, temp_buf);
        serial_write_string(temp_buf);
        serial_write_string("\r\n");
    }
    fb_draw_gradient(boot_info);
    serial_write_string("Main: Gradient background drawn successfully.\r\n");

    /* ---- Draw Terminal Window ---- */
    serial_write_string("Main: Drawing terminal window...\r\n");
    draw_terminal_window(boot_info);
    serial_write_string("Main: Terminal window drawn successfully.\r\n");

    cursor_x = 0;
    cursor_y = 0;

    char input_buf[64];
    int input_len = 0;
    input_buf[0] = '\0';
    bool shift_pressed = false;
    bool needs_redraw = true;
    const char *prompt = "wynland ~ ";

    /* Load persistent command history and set cursor */
    load_history();
    input_cursor = 0;

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

    static bool was_gui_active = false;
    while (1) {
        extern bool wm_is_gui_active(void);
        extern void wm_draw_desktop(void);

        /* Refresh GUI if active */
        if (wm_is_gui_active()) {
            wm_draw_desktop();
            was_gui_active = true;
        } else if (was_gui_active) {
            /* Transitioning back to CLI mode */
            term_x = 0;
            term_y = 0;
            term_w = boot_info->fb_width;
            term_h = boot_info->fb_height;
            console_start_x = 10;
            console_start_y = 10;
            console_end_x = boot_info->fb_width - 10;
            console_end_y = boot_info->fb_height - 10;
            term_bg_color = 0x000F0F1A; // default term bg

            /* Direct framebuffer clear */
            mouse_hide();
            for (uint32_t y = console_start_y; y < console_end_y; y++) {
                uint32_t *row_ptr = (uint32_t *)((uint8_t *)(uintptr_t)boot_info->fb_addr + y * boot_info->fb_pitch);
                for (uint32_t x = console_start_x; x < console_end_x; x++) {
                    row_ptr[x] = term_bg_color;
                }
            }
            cursor_x = 0;
            cursor_y = 0;
            mouse_show();

            console_print_string(boot_info, "Returned to WynlandOS CLI.\n", 0x0088C0D0, term_bg_color);
            console_print_string(boot_info, prompt, 0x00886EFF, term_bg_color);
            was_gui_active = false;
        }

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

        if (wm_is_gui_active()) {
            extern bool wm_is_terminal_focused(void);
            if (!wm_is_terminal_focused()) {
                if (sc == 0xE0) {
                    /* Handle extended scancodes (arrows, etc.) */
                    int timeout = 100000;
                    while (!keyboard_has_scancode() && timeout > 0) {
                        timeout--;
                        __asm__ volatile("nop");
                    }
                    uint8_t sc2 = keyboard_has_scancode() ? keyboard_pop_scancode() : 0;
                    if (sc2 != 0) {
                        extern void wm_handle_key(uint8_t scancode, char ascii);
                        wm_handle_key(sc2, 0);
                    }
                } else if (sc > 0) {
                    extern void wm_handle_key(uint8_t scancode, char ascii);
                    
                    /* Keep track of shift and alt state locally so we can translate scancodes correctly */
                    if (sc & 0x80) {
                        uint8_t released_sc = sc & 0x7F;
                        if (released_sc == 0x2A || released_sc == 0x36) {
                            shift_pressed = false;
                        } else if (released_sc == 0x38) {
                            alt_pressed = false;
                        }
                        wm_handle_key(sc, 0);
                    } else {
                        if (sc == 0x2A || sc == 0x36) {
                            shift_pressed = true;
                            if (alt_pressed) {
                                layout_ru = !layout_ru;
                            }
                            wm_handle_key(sc, 0);
                        } else if (sc == 0x38) {
                            alt_pressed = true;
                            if (shift_pressed) {
                                layout_ru = !layout_ru;
                            }
                            wm_handle_key(sc, 0);
                        } else {
                            char ascii = 0;
                            if (sc == 0x1C) {
                                ascii = '\n';
                            } else if (sc == 0x0E) {
                                ascii = '\b';
                            } else if (sc < 59) {
                                if (layout_ru) {
                                    ascii = translate_scancode_ru(sc, shift_pressed);
                                } else {
                                    ascii = shift_pressed ? scancode_to_ascii_upper[sc] : scancode_to_ascii_lower[sc];
                                }
                            }
                            wm_handle_key(sc, ascii);
                        }
                    }
                }
                continue; /* Skip shell loop processing */
            }
        }


        if (serial_char != 0) {
            if (serial_char == '\r' || serial_char == '\n') {
                draw_input_line(boot_info, prompt, input_buf, input_cursor, NULL);
                console_print_string(boot_info, "\n", 0xFFFFFFFF, term_bg_color);
                
                mouse_hide();
                if (input_len > 0) {
                    add_history(input_buf);
                }
                execute_command(boot_info, input_buf);
                mouse_show();
                
                input_buf[0] = '\0';
                input_len = 0;
                input_cursor = 0;
                
                console_print_string(boot_info, prompt, 0x00886EFF, term_bg_color);
                needs_redraw = true;
            } else if (serial_char == '\b' || serial_char == 127) {
                if (input_cursor > 0) {
                    for (int i = input_cursor - 1; i < input_len - 1; i++) {
                        input_buf[i] = input_buf[i + 1];
                    }
                    input_len--;
                    input_cursor--;
                    input_buf[input_len] = '\0';
                    needs_redraw = true;
                }
            } else if (serial_char >= 32 && serial_char <= 126 && input_len < 60) {
                for (int i = input_len; i > input_cursor; i--) {
                    input_buf[i] = input_buf[i - 1];
                }
                input_buf[input_cursor] = serial_char;
                input_len++;
                input_cursor++;
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
                if (sc2 == 0x4D) { /* Right Arrow */
                    if (input_cursor < input_len) {
                        input_cursor++;
                        needs_redraw = true;
                    } else {
                        /* Autocomplete at end of line */
                        const char *sug = find_suggestion(input_buf, input_len);
                        if (sug) {
                            str_copy(input_buf, sug);
                            input_len = str_len(input_buf);
                            input_cursor = input_len;
                            needs_redraw = true;
                        }
                    }
                } else if (sc2 == 0x4B) { /* Left Arrow */
                    if (input_cursor > 0) {
                        input_cursor--;
                        needs_redraw = true;
                    }
                } else if (sc2 == 0x48) { /* Up Arrow: History backward */
                    if (history_count > 0 && history_index > 0) {
                        history_index--;
                        str_copy(input_buf, history[history_index]);
                        input_len = str_len(input_buf);
                        input_cursor = input_len;
                        needs_redraw = true;
                    }
                } else if (sc2 == 0x50) { /* Down Arrow: History forward */
                    if (history_count > 0) {
                        if (history_index < history_count - 1) {
                            history_index++;
                            str_copy(input_buf, history[history_index]);
                            input_len = str_len(input_buf);
                            input_cursor = input_len;
                            needs_redraw = true;
                        } else if (history_index == history_count - 1) {
                            history_index = history_count;
                            input_buf[0] = '\0';
                            input_len = 0;
                            input_cursor = 0;
                            needs_redraw = true;
                        }
                    }
                }
            }
        } else if (sc > 0) {
            if (sc & 0x80) {
                /* Key release */
                uint8_t released_sc = sc & 0x7F;
                if (released_sc == 0x2A || released_sc == 0x36) {
                    shift_pressed = false;
                } else if (released_sc == 0x38) {
                    alt_pressed = false;
                }
            } else {
                /* Key press */
                if (sc == 0x2A || sc == 0x36) {
                    shift_pressed = true;
                    if (alt_pressed) {
                        layout_ru = !layout_ru;
                    }
                } else if (sc == 0x38) {
                    alt_pressed = true;
                    if (shift_pressed) {
                        layout_ru = !layout_ru;
                    }
                } else if (sc == 0x0E) { /* Backspace */
                    if (input_cursor > 0) {
                        for (int i = input_cursor - 1; i < input_len - 1; i++) {
                            input_buf[i] = input_buf[i + 1];
                        }
                        input_len--;
                        input_cursor--;
                        input_buf[input_len] = '\0';
                        needs_redraw = true;
                    }
                } else if (sc == 0x0F) { /* Tab: Autocomplete */
                    const char *sug = find_suggestion(input_buf, input_len);
                    if (sug) {
                        str_copy(input_buf, sug);
                        input_len = str_len(input_buf);
                        input_cursor = input_len;
                        needs_redraw = true;
                    }
                } else if (sc == 177) { /* Enter */
                    /* Handled internally by compiler syntax parser */
                } else if (sc == 0x1C) { /* Enter */
                    /* Draw final input text without suggestion gray suffix */
                    draw_input_line(boot_info, prompt, input_buf, input_cursor, NULL);
                    console_print_string(boot_info, "\n", 0xFFFFFFFF, term_bg_color);

                    mouse_hide();
                    if (input_len > 0) {
                        add_history(input_buf);
                    }
                    execute_command(boot_info, input_buf);
                    mouse_show();

                    input_buf[0] = '\0';
                    input_len = 0;
                    input_cursor = 0;

                    console_print_string(boot_info, prompt, 0x00886EFF, term_bg_color);
                    needs_redraw = true;
                } else {
                    if (sc < 59) {
                        char ascii;
                        if (layout_ru) {
                            ascii = translate_scancode_ru(sc, shift_pressed);
                        } else {
                            ascii = shift_pressed ? scancode_to_ascii_upper[sc] : scancode_to_ascii_lower[sc];
                        }
                        if (ascii >= 32 && input_len < 60) {
                            for (int i = input_len; i > input_cursor; i--) {
                                input_buf[i] = input_buf[i - 1];
                            }
                            input_buf[input_cursor] = ascii;
                            input_len++;
                            input_cursor++;
                            input_buf[input_len] = '\0';
                            needs_redraw = true;
                        }
                    }
                }
            }
        }

        if (needs_redraw) {
            const char *sug = find_suggestion(input_buf, input_len);
            draw_input_line(boot_info, prompt, input_buf, input_cursor, sug);
            needs_redraw = false;
        }
    }
}
