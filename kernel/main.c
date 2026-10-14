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
#include <wynland/net.h>
#include <wynland/tcp.h>
#include <wynland/udpsock.h>
#include <wynland/elf.h>
#include <wynland/process.h>
#include <wynland/rtc.h>

/* the screen's back-buffer (drivers/video/backbuffer.c) */
extern void compositor_init(BootInfo *info);

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

static inline uint16_t unicode_to_cp866(uint16_t c) {
    if (c >= 0x0410 && c <= 0x042F) {
        return c - 0x0410 + 0x80;
    }
    if (c >= 0x0430 && c <= 0x043F) {
        return c - 0x0430 + 0xA0;
    }
    if (c >= 0x0440 && c <= 0x044F) {
        return c - 0x0440 + 0xE0;
    }
    if (c == 0x0401) return 0xF0; // Ё
    if (c == 0x0451) return 0xF1; // ё
    return c;
}

static void fb_draw_char(BootInfo *info, uint32_t x, uint32_t y,
                          uint16_t c, uint32_t fg, uint32_t bg)
{
    uint8_t braille_buf[16];
    const uint8_t *glyph;

    uint16_t mapped_c = unicode_to_cp866(c);
    if (mapped_c >= 0x2800 && mapped_c <= 0x28FF) {
        get_braille_glyph((uint8_t)(mapped_c - 0x2800), braille_buf);
        glyph = braille_buf;
    } else if (mapped_c < 256) {
        glyph = font_8x16[mapped_c];
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
        uint8_t nibble = (value >> start) & 0xF;
        if (nibble < 10) {
            buf[pos++] = '0' + nibble;
        } else {
            buf[pos++] = 'A' + (nibble - 10);
        }
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

/* "[boot +N ms] what": where the boot time goes (ms since the PIT
   started; the firmware and loader before that are not counted). */
void serial_write_string(const char *str);   /* defined further down */

static void boot_mark(const char *what)
{
    extern uint64_t timer_get_ms(void);
    char b[24];
    uint_to_str(timer_get_ms(), b);
    serial_write_string("[boot +");
    serial_write_string(b);
    serial_write_string(" ms] ");
    serial_write_string(what);
    serial_write_string("\r\n");
}

/* DHCP, /etc/resolv.conf and timezone detection, off the boot path: they
   wait on the network (an internet round trip for the geo-IP lookup, a
   few seconds of timeouts with no network at all) and used to hold the
   desktop back until they were done. The zone from the last boot is
   already in effect (tz_load_cached()). */
static void boot_network(void *arg)
{
    (void)arg;
    if (net_dhcp_request()) {
        /* vfs_create() only makes the directory entry -- call it
           unconditionally and always rewrite: the lease may differ.
           Interrupts off: this thread must not interleave an ext2 update
           with a syscall's (those run with interrupts off). */
        uint64_t rflags;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
        vfs_create("/etc/resolv.conf");
        VfsFile *rf = vfs_open_flags("/etc/resolv.conf", VFS_O_WRITE | VFS_O_TRUNC);
        if (rf) {
            char ip_str[20];
            net_ip_to_str(net_get_dns(), ip_str);
            char line[64];
            uint32_t pos = 0;
            for (const char *q = "nameserver "; *q; q++) line[pos++] = *q;
            for (const char *q = ip_str; *q; q++) line[pos++] = *q;
            line[pos++] = '\n';
            vfs_write(rf, line, pos);
            vfs_close(rf);
        }
        if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
        boot_mark("network up (DHCP)");
        tz_auto_detect();
        boot_mark("timezone detected");
    } else {
        boot_mark("no network");
    }
}

static bool str_contains(const char *str, const char *needle)
{
    for (; *str; str++)
        if (str_starts_with(str, needle)) return true;
    return false;
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
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    
    size_t qwords = n / 8;
    if (qwords > 0) {
        __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(qwords) :: "memory");
    }
    size_t dwords = (n % 8) / 4;
    if (dwords > 0) {
        __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(dwords) :: "memory");
    }
    size_t bytes = n % 4;
    if (bytes > 0) {
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(bytes) :: "memory");
    }
    return dest;
}

void *memset(void *s, int c, size_t n)
{
    uint8_t *d = (uint8_t *)s;
    uint64_t val = (uint8_t)c;
    val |= (val << 8);
    val |= (val << 16);
    val |= (val << 32);

    size_t qwords = n / 8;
    if (qwords > 0) {
        __asm__ volatile("rep stosq" : "+D"(d), "+c"(qwords) : "a"(val) : "memory");
    }
    size_t dwords = (n % 8) / 4;
    if (dwords > 0) {
        __asm__ volatile("rep stosl" : "+D"(d), "+c"(dwords) : "a"((uint32_t)val) : "memory");
    }
    size_t bytes = n % 4;
    if (bytes > 0) {
        __asm__ volatile("rep stosb" : "+D"(d), "+c"(bytes) : "a"((uint8_t)c) : "memory");
    }
    return s;
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

bool serial_received(void)
{
    return inb(PORT_COM1 + 5) & 1;
}

char serial_read_char(void)
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
    cursor_y--;
}

/* Headless app-verification flag: while set, /dev/tty writes skip
   framebuffer drawing and go to serial only. TCG emulation costs ~2.5s
   per full-screen console scroll, which makes any userspace printf run
   minutes per line; real KVM/hardware is unaffected. Set only around
   the boot-time app regression hook. */
bool g_quiet_console = false;

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

void sys_reboot(void)
{
    extern bool ext2_flush(void);
    ext2_flush();   /* write-back cache: nothing may be left behind */
    /* 1. Pulse CPU reset via keyboard controller (port 0x64) */
    uint8_t temp = 0x02;
    while (temp & 0x02) {
        temp = inb(0x64);
    }
    outb(0x64, 0xFE);
    
    /* 2. Fallback: Trigger triple fault by loading an invalid IDT */
    __asm__ volatile("lidt %0; int3" :: "m"((uint16_t[3]){0, 0, 0}));
}

void sys_poweroff(void)
{
    extern bool ext2_flush(void);
    ext2_flush();
    /* QEMU q35 ACPI shutdown */
    outw(0x604, 0x2000);
    /* QEMU piix4 ACPI shutdown */
    outw(0xB004, 0x2000);
    /* VirtualBox / older QEMU ACPI shutdown */
    outw(0x4004, 0x3400);
}

extern void thread_enter_user_mode(void (*entry)(void), void *stack); /* gdt_flush.asm */

void user_exec_wrapper(void *arg) {
    typedef struct {
        void *entry;
        void *stack;
    } ExecArg;
    ExecArg *earg = (ExecArg *)arg;
    void *entry = earg->entry;
    void *stack = earg->stack;
    kfree(earg);
    
    thread_enter_user_mode((void (*)(void))entry, stack);
}

void kernel_main(BootInfo *boot_info)
{
    /* ---- Validate the boot info magic ---- */
    if (!boot_info || boot_info->magic != WYNLAND_BOOT_MAGIC) {
        while (1) {
            __asm__ volatile("hlt");
        }
    }

    /* ---- Enable SSE on CPU ---- */
    {
        uint64_t cr0, cr4;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~(1ULL << 2); // Clear EM (bit 2) - emulation disabled
        cr0 |= (1ULL << 1);  // Set MP (bit 1) - monitor co-processor
        /* WP (bit 16): ring-0 writes honour read-only PTEs too. Without it
           a kernel store into a fork()ed process's COW page (any user
           pointer written without user_prepare_write()) went straight into
           the frame still shared with the parent -- glibc in the parent
           then died of "stack smashing detected". The page-fault handler
           resolves such kernel-mode COW faults (kernel/idt.c). The
           firmware's value was inherited until now. */
        serial_write_string((cr0 & (1ULL << 16)) ? "CR0: WP was already set\r\n"
                                                 : "CR0: WP was clear, setting it\r\n");
        cr0 |= (1ULL << 16);
        __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));

        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 9);  // Set OSFXSR (bit 9) - enable fxsave/fxrstor
        cr4 |= (1ULL << 10); // Set OSXMMEXCPT (bit 10) - enable unmasked SSE exceptions
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
        fpu_init();          // XSAVE + AVX when the CPU has them
    }

    /* ---- Initialize IDT ---- */
    idt_init(boot_info);

    /* ---- Initialize PMM ---- */
    pmm_init(boot_info);

    /* ---- Initialize VMM ---- */
    vmm_init(boot_info);

    /* ---- Initialize Heap ---- */
    heap_init();

    /* ---- Initialize GDT & TSS ---- */
    extern void gdt_init(void);
    gdt_init();

    /* ---- Initialize Syscalls ---- */
    extern void syscall_init(void);
    syscall_init();

    /* ---- Initialize Scheduler ---- */
    sched_init();

    /* ---- Initialize Process 0 (kernel) and wire it to Thread 0 ---- */
    process_init();

    /* ---- Initialize IRQs ---- */
    irq_init();
    boot_mark("interrupts on");

    /* ---- Real wall-clock time: read the CMOS RTC now that the PIT (used
       to track elapsed time since this read) is running. Fixes the
       long-standing "OS thinks it's some default past time" gap --
       needed for real HTTPS cert-date validation and correct file
       timestamps. Timezone auto-detection happens later, once the
       network is up (see net_dhcp_request() below). ---- */
    rtc_init();

    /* ---- Initialize Mouse ---- */
    mouse_init(boot_info);

    /* ---- The screen's back-buffer (virtio-gpu scans it) ---- */
    compositor_init(boot_info);

    /* ---- Initialize VirtIO-GPU ---- */
    extern bool virtio_gpu_init(void);
    virtio_gpu_init();
    boot_mark("virtio-gpu up");

    /* ---- Initialize Serial Port (COM1) ---- */
    serial_init();
    boot_mark("serial up");
    serial_write_string("\r\nWynlandOS Kernel Starting...\r\n");

    /* ---- Initialize Filesystem ---- */
    vfs_init();
    boot_mark("vfs (ext2) mounted");
    {   /* ext2 writes are write-back: this thread puts them on the disk */
        extern void ext2_flusher_thread(void *arg);
        thread_create(ext2_flusher_thread, NULL);
    }
    {   /* alarm(), setitimer(): SIGALRM when due */
        extern void itimer_thread(void *arg);
        thread_create(itimer_thread, NULL);
    }
    /* The image ships these (ext2_manifest.txt); create only what is
       missing. Creating -- even a no-op on an existing name -- walked the
       ext2 bitmaps and cost ~1.5 s each, 9 s of every boot. */
    {
        static const char *const dirs[] = { "/apps", "/docs", "/dev", "/dev/dri" };
        static const char *const files[] = { "/dev/dri/card0", "/dev/dri/renderD128" };
        VfsStat st;
        for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++)
            if (!vfs_stat(dirs[i], &st)) vfs_mkdir(dirs[i]);
        for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++)
            if (!vfs_stat(files[i], &st)) vfs_create(files[i]);
    }

    /* Phase 5: the one concrete protected resource demonstrating the new
       real permission boundary -- created writable, populated, THEN
       marked FAT32_ATTR_READONLY (must write the content before locking
       it, since vfs_set_readonly() only flips the on-disk attribute bit,
       it doesn't itself bypass write-protection). A natural bootstrapping
       example: changing the elevation password later requires already
       being root.
       Name is "sudopw" (6 chars), not "wynpasswd" -- FAT32 8.3 short
       names cap the name portion at 8 characters, vfs_create() doesn't
       generate an LFN entry, and "wynpasswd" (9 chars) silently produced
       a directory entry that a same-name lookup could never find again
       (caught live: vfs_create() reported success, the very next
       vfs_lookup_path() call for the identical path failed). */
    boot_mark("dev dirs");
    vfs_mkdir("/etc"); /* no-op if it already exists, matching the /dev calls above */
    /* (the root password lives in /etc/shadow now, set by `ary login`) */

    boot_mark("sudopw");
    /* ---- Initialize Network Stack ---- */
    net_init();
    boot_mark("net_init done");
    {
        extern void hda_init(void);
        hda_init();
        boot_mark("sound (HDA)");
    }
    tcp_init();
    udp_socket_init();

    /* Automatically request a DHCP lease and write /etc/resolv.conf so
       real Ring-3 programs' standard DNS resolvers (musl's getaddrinfo(),
       via a real socket()+sendto()/recvfrom() UDP round-trip) can find
       a real nameserver -- matching how every real Linux system boots,
       rather than requiring a manual "dhcp" shell command first.
       Best-effort: if there's no network configured for this boot (e.g.
       QEMU launched with no NIC), net_dhcp_request() just returns false
       and nothing else regresses -- apps simply have no DNS, same as
       before this phase existed. */
    boot_mark("filesystem + network stack ready");
    tz_load_cached();
    {
        /* facts for userspace (wynrc service conditions): what this boot
           found, next to the user's own choices in boot.cfg */
        extern bool g_virgl;
        const char *line = g_virgl ? "virgl=1\n" : "virgl=0\n";
        char cur[16] = {0};
        VfsFile *old = vfs_open("/etc/wynland/hw");
        if (old) { vfs_read(old, cur, 8); vfs_close(old); }
        if (!str_starts_with(cur, line)) {
            VfsStat hw_st;
            if (!vfs_stat("/etc/wynland/hw", &hw_st)) vfs_create("/etc/wynland/hw");
            VfsFile *hw = vfs_open_flags("/etc/wynland/hw", VFS_O_WRITE | VFS_O_TRUNC);
            if (hw) { vfs_write(hw, line, 8); vfs_close(hw); }
        }
    }
    {
        extern Thread *thread_create(void (*entry)(void *), void *arg);
        thread_create(boot_network, NULL);
    }

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
    boot_mark("gradient drawn");
    serial_write_string("Main: Gradient background drawn successfully.\r\n");

    /* ---- Draw Terminal Window ---- */
    serial_write_string("Main: Drawing terminal window...\r\n");
    draw_terminal_window(boot_info);
    boot_mark("console window drawn");
    serial_write_string("Main: Terminal window drawn successfully.\r\n");

    cursor_x = 0;
    cursor_y = 0;

    /* Drain keyboard controller buffer from BIOS/UEFI legacy inputs with timeout */
    int drain_timeout = 1000;
    while ((inb(0x64) & 1) && drain_timeout > 0) {
        uint8_t val = inb(0x60);
        if (val == 0xFF) {
            break; /* No keyboard controller or error */
        }
        drain_timeout--;
    }

    /* Hyprland (/hyprland.wyn) is no longer started at boot: the port is
       abandoned (it crashed in its dynamic loader before drawing); Zerp
       is the desktop, Zerp 2.0 goes Qt Quick. Without /sbin/init, uid=1000 below is the
       demotion point -- the kernel (uid 0) hands off to a normal-user
       session, like an init spawning a login shell. */

    /* Zerp v0 -- additive alongside hyprland.wyn for now (Phase 4
       verification), not yet the default launch. hyprland.wyn crashes
       early during its own dynamic-loader mmap, well before drawing
       anything, so it doesn't race with Zerp's /dev/fb0 writes in
       practice. */
    console_print_string(boot_info, "Auto-launching Zerp...\n", 0x0000FF00, term_bg_color);
    boot_mark("starting userspace");
    {
        VfsFile *initf = vfs_open("/sbin/init");
        if (initf) {
            /* wynrc (apps/wynrc): services, their dependencies and the
               runlevels live in /etc/wynrc; it decides what starts. */
            vfs_close(initf);
            console_print_string(boot_info, "Starting wynrc (/sbin/init)...\n", 0x0000FF00, term_bg_color);
            process_spawn("/sbin/init", NULL, 0);   /* init is root; services choose their user */
        } else {
    /* /etc/wynland/boot.cfg (written by `make`, see ZERP/AUTOTEST there):
           zerp=2 -> Zerp 2.0 (Qt Quick on the GPU) when virgl is up and it is
           installed, else the classic Zerp; autotest=1 -> the test programs
           below. Missing file: classic Zerp, no tests. */
        {
            extern bool g_virgl;
            char cfg[256] = {0};
            VfsFile *cf = vfs_open("/etc/wynland/boot.cfg");
            if (cf) { vfs_read(cf, cfg, sizeof(cfg) - 1); vfs_close(cf); }
            bool want_zerp2 = str_contains(cfg, "zerp=2");
            bool autotest   = str_contains(cfg, "autotest=1");
            VfsFile *z2 = want_zerp2 && g_virgl && !autotest ? vfs_open("/usr/bin/zerp2") : NULL;
            if (z2) {
                vfs_close(z2);
                console_print_string(boot_info, "Starting Zerp 2.0...\n", 0x0000FF00, term_bg_color);
                process_spawn("/usr/bin/zerp2", NULL, 1000);
            } else {
                process_spawn("/zerp.elf", NULL, 1000);
            }
    
            /* end-to-end checks: virgl rendering (gltest), process lifecycle +
               networking (forktest), KMS page flips (kmstest). kmstest takes
               the display over, so tests boot with the classic Zerp. */
            if (autotest) {
                if (g_virgl) process_spawn("/gltest.elf", NULL, 1000);
                process_spawn("/forktest.elf", NULL, 1000);
                if (g_virgl) process_spawn("/kmstest.elf", NULL, 1000);
            }
        }
        }
    }

    /* The kernel's own thread idles from here on: the system is
       userspace's (init, the display server, the user's programs). There
       used to be a shell here reading every key typed -- in Zerp too --
       and running it as a kernel command. */
    for (;;) __asm__ volatile("hlt");
}
