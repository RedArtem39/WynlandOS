/*
 * WynlandOS - Framebuffer Compositor Implementation
 * ============================================================
 * Optimized double-buffered compositor with:
 *   1. Bounded Dirty Rectangles (Partial screen updates)
 *   2. Lightweight Cursor Save/Restore path (0% CPU mouse moves)
 *   3. Inline pixel raw drawing to bypass tracking inside loops
 */

#include "compositor.h"
#include "theme.h"
#include <wynland/heap.h>
#include <wynland/font.h>
#include <wynland/irq.h>

extern void serial_write_string(const char *str);
extern void uint_to_str(uint64_t val, char *buf);
extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);

/* ---- Global compositor instance ---- */
static Compositor g_comp;

/* ---- Bounded Dirty Rectangle tracking ---- */
static int32_t g_dirty_x1 = 999999;
static int32_t g_dirty_y1 = 999999;
static int32_t g_dirty_x2 = -999999;
static int32_t g_dirty_y2 = -999999;

/* ---- Cursor Save/Restore buffers ---- */
static uint32_t cursor_save_buffer[19 * 18];
static int32_t saved_cursor_x = -1;
static int32_t saved_cursor_y = -1;
static bool has_saved_cursor = false;

/* ============================================================
 * Dirty Bounds & Cursor Helpers
 * ============================================================ */

void comp_mark_area_dirty(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= (int32_t)g_comp.fb_width) x2 = g_comp.fb_width - 1;
    if (y2 >= (int32_t)g_comp.fb_height) y2 = g_comp.fb_height - 1;

    if (x1 <= x2 && y1 <= y2) {
        if (x1 < g_dirty_x1) g_dirty_x1 = x1;
        if (y1 < g_dirty_y1) g_dirty_y1 = y1;
        if (x2 > g_dirty_x2) g_dirty_x2 = x2;
        if (y2 > g_dirty_y2) g_dirty_y2 = y2;
        g_comp.dirty = true;
    }
}

void comp_clear_saved_cursor(void)
{
    has_saved_cursor = false;
}

void comp_restore_cursor_back(void)
{
    if (!has_saved_cursor) return;

    uint32_t bw = g_comp.fb_width;
    uint32_t bh = g_comp.fb_height;
    uint32_t *buf = g_comp.back_buffer;

    for (int y = 0; y < 19; y++) {
        int32_t screen_y = saved_cursor_y + y;
        if (screen_y >= (int32_t)bh) break;
        for (int x = 0; x < 18; x++) {
            int32_t screen_x = saved_cursor_x + x;
            if (screen_x >= (int32_t)bw) break;

            buf[screen_y * bw + screen_x] = cursor_save_buffer[y * 18 + x];
        }
    }

    comp_mark_area_dirty(saved_cursor_x, saved_cursor_y, saved_cursor_x + 17, saved_cursor_y + 18);
    has_saved_cursor = false;
}

void comp_save_cursor_back(int32_t mx, int32_t my)
{
    if (has_saved_cursor) {
        comp_restore_cursor_back();
    }

    uint32_t bw = g_comp.fb_width;
    uint32_t bh = g_comp.fb_height;
    uint32_t *buf = g_comp.back_buffer;

    saved_cursor_x = mx;
    saved_cursor_y = my;
    has_saved_cursor = true;

    for (int y = 0; y < 19; y++) {
        int32_t screen_y = my + y;
        if (screen_y >= (int32_t)bh) {
            for (int x = 0; x < 18; x++) {
                cursor_save_buffer[y * 18 + x] = 0x2E3440;
            }
            continue;
        }
        for (int x = 0; x < 18; x++) {
            int32_t screen_x = mx + x;
            if (screen_x >= (int32_t)bw) {
                cursor_save_buffer[y * 18 + x] = 0x2E3440;
                continue;
            }
            cursor_save_buffer[y * 18 + x] = buf[screen_y * bw + screen_x];
        }
    }
}

/* ============================================================
 * Lifecycle
 * ============================================================ */

void compositor_init(BootInfo *info)
{
    serial_write_string("Compositor: Initializing double-buffered compositor...\r\n");

    g_comp.boot_info    = info;
    g_comp.front_buffer = (uint32_t *)(uintptr_t)info->fb_addr;
    g_comp.fb_width     = info->fb_width;
    g_comp.fb_height    = info->fb_height;
    g_comp.fb_pitch     = info->fb_pitch;
    g_comp.dirty        = true;
    g_comp.frame_count  = 0;

    /* Allocate back-buffer */
    uint32_t buf_size = g_comp.fb_width * g_comp.fb_height * 4;
    g_comp.back_buffer = (uint32_t *)kmalloc(buf_size);

    if (!g_comp.back_buffer) {
        serial_write_string("Compositor: ERROR - failed to allocate back-buffer!\r\n");
        return;
    }

    /* Clear back-buffer to desktop background color */
    uint32_t pixel_count = g_comp.fb_width * g_comp.fb_height;
    uint32_t bg = THEME_DESKTOP_TOP & 0x00FFFFFF;  /* strip alpha for FB */
    for (uint32_t i = 0; i < pixel_count; i++) {
        g_comp.back_buffer[i] = bg;
    }

    /* Set initial full dirty rectangle */
    g_dirty_x1 = 0;
    g_dirty_y1 = 0;
    g_dirty_x2 = g_comp.fb_width - 1;
    g_dirty_y2 = g_comp.fb_height - 1;

    char buf[32];
    serial_write_string("Compositor: Back-buffer allocated (");
    uint_to_str(buf_size / 1024, buf);
    serial_write_string(buf);
    serial_write_string(" KB)\r\n");
    serial_write_string("Compositor: Initialized successfully.\r\n");
}

#include <wynland/mouse.h>

void compositor_flip(void)
{
    if (!g_comp.back_buffer || !g_comp.dirty) return;

    /* Clamp dirty bounds to screen boundaries */
    int32_t x1 = g_dirty_x1;
    int32_t y1 = g_dirty_y1;
    int32_t x2 = g_dirty_x2;
    int32_t y2 = g_dirty_y2;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= (int32_t)g_comp.fb_width) x2 = g_comp.fb_width - 1;
    if (y2 >= (int32_t)g_comp.fb_height) y2 = g_comp.fb_height - 1;

    if (x1 <= x2 && y1 <= y2) {
        uint32_t pitch_pixels = g_comp.fb_pitch / 4;
        uint32_t w = g_comp.fb_width;
        uint32_t copy_width_bytes = (x2 - x1 + 1) * 4;

        for (int32_t y = y1; y <= y2; y++) {
            memcpy(&g_comp.front_buffer[y * pitch_pixels + x1],
                   &g_comp.back_buffer[y * w + x1],
                   copy_width_bytes);
        }
    }

    /* Reset dirty bounds */
    g_dirty_x1 = 999999;
    g_dirty_y1 = 999999;
    g_dirty_x2 = -999999;
    g_dirty_y2 = -999999;
    g_comp.dirty = false;
    g_comp.frame_count++;
}

/* ============================================================
 * Drawing Primitives (into back-buffer)
 * ============================================================ */

static inline void comp_draw_pixel_raw(uint32_t x, uint32_t y, uint32_t color)
{
    if (x >= g_comp.fb_width || y >= g_comp.fb_height) return;
    g_comp.back_buffer[y * g_comp.fb_width + x] = color;
}

void comp_draw_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    comp_draw_pixel_raw(x, y, color);
    comp_mark_area_dirty(x, y, x, y);
}

void comp_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    uint32_t bw = g_comp.fb_width;
    uint32_t bh = g_comp.fb_height;

    /* Clamp */
    if (x >= bw || y >= bh) return;
    if (x + w > bw) w = bw - x;
    if (y + h > bh) h = bh - y;

    for (uint32_t row = 0; row < h; row++) {
        uint32_t *dst = &g_comp.back_buffer[(y + row) * bw + x];
        for (uint32_t col = 0; col < w; col++) {
            dst[col] = color;
        }
    }
    comp_mark_area_dirty(x, y, x + w - 1, y + h - 1);
}

/* Alpha-blended fill: color is 0xAARRGGBB */
void comp_fill_rect_alpha(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb)
{
    uint32_t bw = g_comp.fb_width;
    uint32_t bh = g_comp.fb_height;

    if (x >= bw || y >= bh) return;
    if (x + w > bw) w = bw - x;
    if (y + h > bh) h = bh - y;

    uint32_t alpha = (argb >> 24) & 0xFF;
    uint32_t src_r = (argb >> 16) & 0xFF;
    uint32_t src_g = (argb >>  8) & 0xFF;
    uint32_t src_b = (argb      ) & 0xFF;

    /* Pre-multiply for speed */
    uint32_t inv_alpha = 255 - alpha;

    for (uint32_t row = 0; row < h; row++) {
        uint32_t *dst = &g_comp.back_buffer[(y + row) * bw + x];
        for (uint32_t col = 0; col < w; col++) {
            uint32_t bg = dst[col];
            uint32_t bg_r = (bg >> 16) & 0xFF;
            uint32_t bg_g = (bg >>  8) & 0xFF;
            uint32_t bg_b = (bg      ) & 0xFF;

            uint32_t out_r = (src_r * alpha + bg_r * inv_alpha) / 255;
            uint32_t out_g = (src_g * alpha + bg_g * inv_alpha) / 255;
            uint32_t out_b = (src_b * alpha + bg_b * inv_alpha) / 255;

            dst[col] = (out_r << 16) | (out_g << 8) | out_b;
        }
    }
    comp_mark_area_dirty(x, y, x + w - 1, y + h - 1);
}

void comp_box_blur(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t radius)
{
    uint32_t bw = g_comp.fb_width;
    uint32_t bh = g_comp.fb_height;
    uint32_t *buf = g_comp.back_buffer;

    if (x >= bw || y >= bh) return;
    if (x + w > bw) w = bw - x;
    if (y + h > bh) h = bh - y;
    if (w > 1024) w = 1024;

    /* Use a stack-allocated row buffer to avoid heap kmalloc/kfree overhead */
    uint32_t temp_row[1024];

    for (uint32_t row = 0; row < h; row++) {
        uint32_t py = y + row;
        
        for (uint32_t col = 0; col < w; col++) {
            uint32_t px = x + col;
            uint32_t r_sum = 0, g_sum = 0, b_sum = 0, count = 0;

            for (int32_t ky = -(int32_t)radius; ky <= (int32_t)radius; ky++) {
                int32_t ny = (int32_t)py + ky;
                if (ny < 0 || ny >= (int32_t)bh) continue;

                for (int32_t kx = -(int32_t)radius; kx <= (int32_t)radius; kx++) {
                    int32_t nx = (int32_t)px + kx;
                    if (nx < 0 || nx >= (int32_t)bw) continue;

                    uint32_t pixel = buf[ny * bw + nx];
                    r_sum += (pixel >> 16) & 0xFF;
                    g_sum += (pixel >> 8) & 0xFF;
                    b_sum += pixel & 0xFF;
                    count++;
                }
            }

            if (count > 0) {
                temp_row[col] = ((r_sum / count) << 16) | ((g_sum / count) << 8) | (b_sum / count);
            } else {
                temp_row[col] = buf[py * bw + px];
            }
        }
        
        /* Copy blurred row back */
        memcpy(&buf[py * bw + x], temp_row, w * 4);
    }

    comp_mark_area_dirty(x, y, x + w - 1, y + h - 1);
}

void comp_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg)
{
    const uint8_t *glyph = font_8x16[(uint8_t)c];

    for (uint32_t row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                comp_draw_pixel_raw(x + col, y + row, fg & 0x00FFFFFF);
            } else if (bg != 0) {
                comp_draw_pixel_raw(x + col, y + row, bg & 0x00FFFFFF);
            }
        }
    }
    comp_mark_area_dirty(x, y, x + 7, y + 15);
}

void comp_draw_string(uint32_t x, uint32_t y, const char *str, uint32_t fg, uint32_t bg)
{
    uint32_t start_x = x;
    while (*str) {
        if (*str == '\n') {
            x = start_x;
            y += 18;
        } else {
            comp_draw_char(x, y, *str, fg, bg);
            x += 9;
        }
        str++;
    }
}

void comp_draw_circle(uint32_t cx, uint32_t cy, uint32_t r, uint32_t color)
{
    uint32_t c = color & 0x00FFFFFF;
    for (int32_t dy = -(int32_t)r; dy <= (int32_t)r; dy++) {
        for (int32_t dx = -(int32_t)r; dx <= (int32_t)r; dx++) {
            if (dx * dx + dy * dy <= (int32_t)(r * r)) {
                comp_draw_pixel_raw(cx + dx, cy + dy, c);
            }
        }
    }
    comp_mark_area_dirty(cx - r, cy - r, cx + r, cy + r);
}

static void comp_draw_pixel_alpha(uint32_t x, uint32_t y, uint32_t argb)
{
    if (x >= g_comp.fb_width || y >= g_comp.fb_height) return;
    uint32_t alpha = (argb >> 24) & 0xFF;
    if (alpha == 255) {
        g_comp.back_buffer[y * g_comp.fb_width + x] = argb & 0x00FFFFFF;
    } else if (alpha > 0) {
        uint32_t bg = g_comp.back_buffer[y * g_comp.fb_width + x];
        uint32_t bg_r = (bg >> 16) & 0xFF;
        uint32_t bg_g = (bg >>  8) & 0xFF;
        uint32_t bg_b = (bg      ) & 0xFF;
        
        uint32_t src_r = (argb >> 16) & 0xFF;
        uint32_t src_g = (argb >>  8) & 0xFF;
        uint32_t src_b = (argb      ) & 0xFF;
        
        uint32_t inv_alpha = 255 - alpha;
        uint32_t out_r = (src_r * alpha + bg_r * inv_alpha) / 255;
        uint32_t out_g = (src_g * alpha + bg_g * inv_alpha) / 255;
        uint32_t out_b = (src_b * alpha + bg_b * inv_alpha) / 255;
        
        g_comp.back_buffer[y * g_comp.fb_width + x] = (out_r << 16) | (out_g << 8) | out_b;
    }
}

void comp_draw_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            uint32_t r, uint32_t color)
{
    uint32_t alpha = (color >> 24) & 0xFF;
    uint32_t c = color & 0x00FFFFFF;

    if (alpha > 0 && alpha < 255) {
        /* Fill center area with alpha */
        comp_fill_rect_alpha(x + r, y, w - 2 * r, h, color);
        comp_fill_rect_alpha(x, y + r, r, h - 2 * r, color);
        comp_fill_rect_alpha(x + w - r, y + r, r, h - 2 * r, color);

        /* Draw corners with alpha */
        for (int32_t dy = -(int32_t)r; dy <= 0; dy++) {
            for (int32_t dx = -(int32_t)r; dx <= 0; dx++) {
                if (dx * dx + dy * dy <= (int32_t)(r * r)) {
                    comp_draw_pixel_alpha(x + r + dx, y + r + dy, color);
                    comp_draw_pixel_alpha(x + w - 1 - r - dx, y + r + dy, color);
                    comp_draw_pixel_alpha(x + r + dx, y + h - 1 - r - dy, color);
                    comp_draw_pixel_alpha(x + w - 1 - r - dx, y + h - 1 - r - dy, color);
                }
            }
        }
    } else {
        /* Solid fill */
        comp_fill_rect(x + r, y, w - 2 * r, h, c);
        comp_fill_rect(x, y + r, r, h - 2 * r, c);
        comp_fill_rect(x + w - r, y + r, r, h - 2 * r, c);

        /* Draw corners */
        for (int32_t dy = -(int32_t)r; dy <= 0; dy++) {
            for (int32_t dx = -(int32_t)r; dx <= 0; dx++) {
                if (dx * dx + dy * dy <= (int32_t)(r * r)) {
                    comp_draw_pixel_raw(x + r + dx, y + r + dy, c);
                    comp_draw_pixel_raw(x + w - 1 - r - dx, y + r + dy, c);
                    comp_draw_pixel_raw(x + r + dx, y + h - 1 - r - dy, c);
                    comp_draw_pixel_raw(x + w - 1 - r - dx, y + h - 1 - r - dy, c);
                }
            }
        }
    }
    comp_mark_area_dirty(x, y, x + w - 1, y + h - 1);
}

/* ============================================================
 * Accessors
 * ============================================================ */

uint32_t *comp_get_backbuffer(void) { return g_comp.back_buffer;  }
uint32_t  comp_get_width(void)      { return g_comp.fb_width;     }
uint32_t  comp_get_height(void)     { return g_comp.fb_height;    }

void comp_mark_dirty(void)
{
    comp_mark_area_dirty(0, 0, g_comp.fb_width - 1, g_comp.fb_height - 1);
    extern void wm_mark_dirty(void);
    wm_mark_dirty();
}

/* ============================================================
 * Desktop Wallpaper (Nord gradient)
 * ============================================================ */

void comp_draw_wallpaper(void)
{
    uint32_t w = g_comp.fb_width;
    uint32_t h = g_comp.fb_height;

    /* Nord gradient: NORD0 (#2E3440) at top → slightly darker at bottom */
    uint32_t top_r = 0x2E, top_g = 0x34, top_b = 0x40;
    uint32_t bot_r = 0x23, bot_g = 0x28, bot_b = 0x31;

    for (uint32_t y = 0; y < h; y++) {
        uint32_t r = top_r + (bot_r - top_r) * y / h;
        uint32_t g = top_g + (bot_g - top_g) * y / h;
        uint32_t b = top_b + (bot_b - top_b) * y / h;

        uint32_t *row = &g_comp.back_buffer[y * w];
        for (uint32_t x = 0; x < w; x++) {
            /* Add a very subtle radial vignette */
            int32_t cx = (int32_t)x - (int32_t)(w / 2);
            int32_t cy = (int32_t)y - (int32_t)(h / 2);
            int32_t dist_sq = cx * cx + cy * cy;
            int32_t max_dist = (int32_t)((w/2) * (w/2) + (h/2) * (h/2));

            /* Darken edges by up to 15% */
            int32_t darken = (dist_sq * 40) / max_dist;
            if (darken > 40) darken = 40;

            int32_t pr = (int32_t)r - darken;
            int32_t pg = (int32_t)g - darken;
            int32_t pb = (int32_t)b - darken;
            if (pr < 0) pr = 0;
            if (pg < 0) pg = 0;
            if (pb < 0) pb = 0;

            row[x] = ((uint32_t)pr << 16) | ((uint32_t)pg << 8) | (uint32_t)pb;
        }
    }
    comp_mark_area_dirty(0, 0, w - 1, h - 1);
}

/* ============================================================
 * Top Panel (macOS-style menu bar)
 * ============================================================ */

void comp_draw_panel(void)
{
    uint32_t w = g_comp.fb_width;
    uint32_t panel_h = THEME_PANEL_HEIGHT;

    /* Draw semi-transparent panel background */
    comp_fill_rect_alpha(0, 0, w, panel_h, THEME_PANEL_BG);

    /* Draw subtle bottom separator line */
    comp_fill_rect(0, panel_h - 1, w, 1, NORD3 & 0x00FFFFFF);

    /* Left side: WynlandOS logo / name */
    uint32_t fg = THEME_TEXT_PRIMARY & 0x00FFFFFF;
    comp_draw_string(10, 4, "W", THEME_ACCENT & 0x00FFFFFF, 0);
    comp_draw_string(19, 4, "ynlandOS", fg, 0);

    /* Right side: placeholder clock */
    comp_draw_string(w - 80, 4, "00:00:00", fg, 0);
}

void comp_draw_cursor(int32_t mx, int32_t my, uint8_t buttons)
{
    /* 1. Save background under cursor */
    comp_save_cursor_back(mx, my);

    /* 2. Draw the cursor mask onto the back-buffer */
    uint32_t bw = g_comp.fb_width;
    uint32_t bh = g_comp.fb_height;
    uint32_t *buf = g_comp.back_buffer;

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

    for (int y = 0; y < 19; y++) {
        int32_t screen_y = my + y;
        if (screen_y >= (int32_t)bh) break;
        for (int x = 0; x < 18; x++) {
            int32_t screen_x = mx + x;
            if (screen_x >= (int32_t)bw) break;

            uint8_t pixel_type = cursor_mask[y][x];
            if (pixel_type == 1) {
                buf[screen_y * bw + screen_x] = 0x00000000; /* Black outline */
            } else if (pixel_type == 2) {
                if (buttons & 1) {
                    buf[screen_y * bw + screen_x] = 0x00FF3333; /* Red on Left Click */
                } else if (buttons & 2) {
                    buf[screen_y * bw + screen_x] = 0x0033FF33; /* Green on Right Click */
                } else {
                    buf[screen_y * bw + screen_x] = 0x00FFFFFF; /* White */
                }
            }
        }
    }

    /* 3. Mark the new cursor area as dirty */
    comp_mark_area_dirty(mx, my, mx + 17, my + 18);
}
