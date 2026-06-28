#include <wynland/types.h>
#include <wynland/font.h>

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_mmap 9
#define SYS_exit 60
#define SYS_mark_dirty 401
#define SYS_kfree 404

/* Globals */
uint32_t *g_fb_ptr = (uint32_t*)1;
void *g_status_label = (void*)1;
bool g_btn_state = true;

/* Helper declarations */
long syscall_raw(long num, long a1, long a2, long a3, long a4, long a5);
int sys_open(const char *path, uint64_t flags);
void sys_close(int fd);
int sys_read(int fd, void *buf, uint64_t size);
int sys_write(int fd, const void *buf, uint64_t size);
void sys_exit(int code);
void print_string(const char *msg);

/* Compositor drawing functions declarations */
extern "C" {
    void comp_draw_string(uint32_t x, uint32_t y, const char *str, uint32_t fg, uint32_t bg);
    void comp_draw_char_aa(uint32_t x, uint32_t y, uint32_t c, uint32_t fg, int size);
    void comp_draw_string_aa(uint32_t x, uint32_t y, const char *str, uint32_t fg, int size);
    uint32_t comp_string_width_aa(const char *str, int size);
    void comp_draw_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t radius, uint32_t color);
    void comp_draw_rounded_rect_border(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t radius, uint32_t color);
    void comp_mark_dirty(void);
}

/* C++ memory operator declarations */
void* operator new(unsigned long size);
void* operator new[](unsigned long size);
void operator delete(void* ptr) noexcept;
void operator delete[](void* ptr) noexcept;
void operator delete(void* ptr, unsigned long) noexcept;
void operator delete[](void* ptr, unsigned long) noexcept;

/* Include mock Qt headers */
#include "gui/qt.hpp"

/* Forward declarations of callbacks */
QLabel *get_status_label();
void on_button_click();

/* Program entry point MUST be at the very top of code output to align with 0x40000000 */
extern "C" void _start() {
    g_fb_ptr = nullptr;
    g_status_label = nullptr;
    g_btn_state = false;

    print_string("========================================\n");
    print_string("  WynlandOS userspace Qt test starting  \n");
    print_string("========================================\n");

    // 1. Open /dev/fb0
    print_string("Opening /dev/fb0...\n");
    int fb_fd = sys_open("/dev/fb0", 3); // O_RDWR
    if (fb_fd < 0) {
        print_string("Failed to open /dev/fb0!\n");
        sys_exit(1);
    }

    // 2. Map /dev/fb0
    print_string("mmap'ing /dev/fb0...\n");
    uint32_t fb_size = 1920 * 1080 * 4;
    g_fb_ptr = (uint32_t*)syscall_raw(SYS_mmap, 0, fb_size, 0, 0, fb_fd);
    if (!g_fb_ptr) {
        print_string("Failed to mmap /dev/fb0!\n");
        sys_close(fb_fd);
        sys_exit(1);
    }

    // 3. Open /dev/input/mice
    print_string("Opening /dev/input/mice...\n");
    int mouse_fd = sys_open("/dev/input/mice", 1); // O_RDONLY
    if (mouse_fd < 0) {
        print_string("Failed to open /dev/input/mice!\n");
        sys_close(fb_fd);
        sys_exit(1);
    }

    print_string("Initializing Qt Window layout...\n");

    // 4. Construct widget layout
    int32_t win_x = (1920 - 500) / 2;
    int32_t win_y = (1080 - 350) / 2;
    QWidget mainWindow(win_x, win_y, 500, 350);

    QVBoxLayout layout(&mainWindow);

    QLabel titleLabel(" WynlandOS - C++ Qt Port Validation ");
    g_status_label = new QLabel("State: INACTIVE - Click to enable!");
    
    QPushButton actionButton("Toggle State");
    actionButton.setCallback(on_button_click);

    layout.addWidget(&titleLabel);
    layout.addWidget(get_status_label());
    layout.addWidget(&actionButton);

    mainWindow.show();

    // Render initial layout
    comp_draw_rounded_rect(win_x - 4, win_y - 4, 508, 358, 8, 0x002E3440); // Window shadow/border
    comp_draw_rounded_rect(win_x, win_y, 500, 350, 8, 0x003B4252);        // Window body
    mainWindow.paint();
    comp_mark_dirty();

    print_string("Running Qt app event loop! Click the button or Right Click to exit.\n");

    uint8_t packet[3];
    int32_t cursor_x = 960;
    int32_t cursor_y = 540;

    while (1) {
        if (sys_read(mouse_fd, packet, 3) == 3) {
            // Exit on Right Click
            if (packet[0] & 2) {
                print_string("Right click detected. Exiting Qt event loop!\n");
                break;
            }

            int8_t rel_x = (int8_t)packet[1];
            int8_t rel_y = (int8_t)packet[2];
            cursor_x += rel_x;
            cursor_y -= rel_y;

            if (cursor_x < 0) cursor_x = 0;
            if (cursor_x >= 1920) cursor_x = 1919;
            if (cursor_y < 0) cursor_y = 0;
            if (cursor_y >= 1080) cursor_y = 1079;

            // Route mouse events to Qt widgets
            mainWindow.handle_mouse(cursor_x, cursor_y, packet[0]);

            // Redraw window and widgets
            comp_draw_rounded_rect(win_x, win_y, 500, 350, 8, 0x003B4252);
            mainWindow.paint();
            comp_mark_dirty();
        }
    }

    // Clean up
    delete get_status_label();
    sys_close(mouse_fd);
    sys_close(fb_fd);

    print_string("Qt test application closed successfully!\n");
    sys_exit(0);
}

/* Callback implementations */
QLabel *get_status_label() {
    return (QLabel*)g_status_label;
}

void on_button_click() {
    g_btn_state = !g_btn_state;
    if (g_btn_state) {
        get_status_label()->setText("State: ACTIVE  -  Click to disable!");
    } else {
        get_status_label()->setText("State: INACTIVE - Click to enable!");
    }
}

/* Compositor drawing functions implementation */
#include <wynland/font_aa_13.h>
#include <wynland/font_aa_16.h>

extern "C" {
    void comp_draw_pixel_alpha(uint32_t x, uint32_t y, uint32_t argb) {
        if (!g_fb_ptr) return;
        uint32_t alpha = (argb >> 24) & 0xFF;
        if (alpha == 0) return;
        if (alpha == 255) {
            g_fb_ptr[y * 1920 + x] = argb & 0xFFFFFF;
            return;
        }
        uint32_t bg = g_fb_ptr[y * 1920 + x];
        uint32_t r = (((argb >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * (255 - alpha)) / 255;
        uint32_t g = (((argb >> 8) & 0xFF) * alpha + ((bg >> 8) & 0xFF) * (255 - alpha)) / 255;
        uint32_t b = (((argb) & 0xFF) * alpha + ((bg) & 0xFF) * (255 - alpha)) / 255;
        g_fb_ptr[y * 1920 + x] = (r << 16) | (g << 8) | b;
    }

    void comp_draw_char_aa(uint32_t x, uint32_t y, uint32_t c, uint32_t fg, int size) {
        if (size == 16) {
            int idx = get_glyph_index_16(c);
            if (idx < 0) idx = get_glyph_index_16('?');
            if (idx < 0) return;
            
            const GlyphAA_16 *g = &glyphs_16[idx];
            if (g->width == 0 || g->height == 0) return;
            
            for (uint32_t gy = 0; gy < g->height; gy++) {
                uint32_t py = y + g->top + gy;
                if (py >= 1080) continue;
                for (uint32_t gx = 0; gx < g->width; gx++) {
                    uint32_t px = x + g->left + gx;
                    if (px >= 1920) continue;
                    uint8_t alpha = bitmaps_16[g->offset + gy * g->width + gx];
                    if (alpha > 0) {
                        comp_draw_pixel_alpha(px, py, ((uint32_t)alpha << 24) | (fg & 0x00FFFFFF));
                    }
                }
            }
        } else {
            int idx = get_glyph_index_13(c);
            if (idx < 0) idx = get_glyph_index_13('?');
            if (idx < 0) return;
            
            const GlyphAA_13 *g = &glyphs_13[idx];
            if (g->width == 0 || g->height == 0) return;
            
            for (uint32_t gy = 0; gy < g->height; gy++) {
                uint32_t py = y + g->top + gy;
                if (py >= 1080) continue;
                for (uint32_t gx = 0; gx < g->width; gx++) {
                    uint32_t px = x + g->left + gx;
                    if (px >= 1920) continue;
                    uint8_t alpha = bitmaps_13[g->offset + gy * g->width + gx];
                    if (alpha > 0) {
                        comp_draw_pixel_alpha(px, py, ((uint32_t)alpha << 24) | (fg & 0x00FFFFFF));
                    }
                }
            }
        }
    }

    void comp_draw_string_aa(uint32_t x, uint32_t y, const char *str, uint32_t fg, int size) {
        uint32_t cur_x = x;
        while (*str) {
            uint32_t c = (uint8_t)*str;
            
            if (c >= 0xC0) {
                uint8_t b1 = c;
                uint8_t b2 = (uint8_t)*(str + 1);
                if (b2) {
                    c = ((b1 & 0x1F) << 6) | (b2 & 0x3F);
                    str++;
                }
            }
            
            if (c == '\n') {
                cur_x = x;
                y += (size == 16) ? 20 : 16;
            } else {
                int idx = (size == 16) ? get_glyph_index_16(c) : get_glyph_index_13(c);
                uint32_t adv = 8;
                if (idx >= 0) {
                    adv = (size == 16) ? glyphs_16[idx].advance : glyphs_13[idx].advance;
                    comp_draw_char_aa(cur_x, y, c, fg, size);
                }
                cur_x += adv;
            }
            str++;
        }
    }

    uint32_t comp_string_width_aa(const char *str, int size) {
        uint32_t width = 0;
        while (*str) {
            uint32_t c = (uint8_t)*str;
            if (c >= 0xC0) {
                uint8_t b1 = c;
                uint8_t b2 = (uint8_t)*(str + 1);
                if (b2) {
                    c = ((b1 & 0x1F) << 6) | (b2 & 0x3F);
                    str++;
                }
            }
            int idx = (size == 16) ? get_glyph_index_16(c) : get_glyph_index_13(c);
            if (idx >= 0) {
                width += (size == 16) ? glyphs_16[idx].advance : glyphs_13[idx].advance;
            } else {
                width += 8;
            }
            str++;
        }
        return width;
    }

    void comp_draw_string(uint32_t x, uint32_t y, const char *str, uint32_t fg, uint32_t bg) {
        if (!g_fb_ptr) return;
        uint32_t cx = x;
        while (*str) {
            char c = *str;
            const uint8_t *glyph = font_8x16[(uint8_t)c];
            for (uint32_t gy = 0; gy < 16; gy++) {
                uint8_t row = glyph[gy];
                uint32_t py = y + gy;
                if (py >= 1080) continue;
                for (uint32_t gx = 0; gx < 8; gx++) {
                    uint32_t px = cx + gx;
                    if (px >= 1920) continue;
                    if (row & (1 << (7 - gx))) {
                        g_fb_ptr[py * 1920 + px] = fg;
                    } else if (bg != 0) {
                        g_fb_ptr[py * 1920 + px] = bg;
                    }
                }
            }
            cx += 8;
            str++;
        }
    }

    void comp_draw_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t radius, uint32_t color) {
        if (!g_fb_ptr) return;
        for (uint32_t dy = 0; dy < h; dy++) {
            uint32_t py = y + dy;
            if (py >= 1080) continue;
            for (uint32_t dx = 0; dx < w; dx++) {
                uint32_t px = x + dx;
                if (px >= 1920) continue;
                g_fb_ptr[py * 1920 + px] = color;
            }
        }
    }

    void comp_draw_rounded_rect_border(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t radius, uint32_t color) {
        if (!g_fb_ptr) return;
        for (uint32_t dy = 0; dy < h; dy++) {
            uint32_t py = y + dy;
            if (py >= 1080) continue;
            for (uint32_t dx = 0; dx < w; dx++) {
                uint32_t px = x + dx;
                if (px >= 1920) continue;
                if (dy == 0 || dy == h - 1 || dx == 0 || dx == w - 1) {
                    g_fb_ptr[py * 1920 + px] = color;
                }
            }
        }
    }

    void comp_mark_dirty(void) {
        syscall_raw(SYS_mark_dirty, 0, 0, 0, 0, 0);
    }
}

/* Helper implementations */
long syscall_raw(long num, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    __asm__ volatile(
        "movq %1, %%rax\n"
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "movq %5, %%r10\n"
        "movq %6, %%r8\n"
        "syscall\n"
        : "=a"(ret)
        : "g"(num), "g"(a1), "g"(a2), "g"(a3), "g"(a4), "g"(a5)
        : "rdi", "rsi", "rdx", "rcx", "r11", "r10", "r8", "memory"
    );
    return ret;
}

int sys_open(const char *path, uint64_t flags) {
    return (int)syscall_raw(SYS_open, (long)path, flags, 0, 0, 0);
}

void sys_close(int fd) {
    syscall_raw(SYS_close, fd, 0, 0, 0, 0);
}

int sys_read(int fd, void *buf, uint64_t size) {
    return (int)syscall_raw(SYS_read, fd, (long)buf, size, 0, 0);
}

int sys_write(int fd, const void *buf, uint64_t size) {
    return (int)syscall_raw(SYS_write, fd, (long)buf, size, 0, 0);
}

void sys_exit(int code) {
    syscall_raw(SYS_exit, code, 0, 0, 0, 0);
    while (1) {
        __asm__ volatile("hlt");
    }
}

void print_string(const char *msg) {
    int len = 0;
    while (msg[len]) len++;
    sys_write(1, msg, len);
}

void* operator new(unsigned long size) {
    return (void*)syscall_raw(SYS_mmap, 0, size, 0, 0, 0);
}

void* operator new[](unsigned long size) {
    return (void*)syscall_raw(SYS_mmap, 0, size, 0, 0, 0);
}

void operator delete(void* ptr) noexcept {
    syscall_raw(SYS_kfree, (long)ptr, 0, 0, 0, 0);
}

void operator delete[](void* ptr) noexcept {
    syscall_raw(SYS_kfree, (long)ptr, 0, 0, 0, 0);
}

void operator delete(void* ptr, unsigned long) noexcept {
    syscall_raw(SYS_kfree, (long)ptr, 0, 0, 0, 0);
}

void operator delete[](void* ptr, unsigned long) noexcept {
    syscall_raw(SYS_kfree, (long)ptr, 0, 0, 0, 0);
}

/* Pure virtual function handler */
extern "C" void __cxa_pure_virtual() {
    print_string("CRITICAL: Pure virtual function call!\n");
    sys_exit(1);
}
