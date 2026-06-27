/*
 * WynlandOS - Framebuffer Compositor Implementation
 * ============================================================
 * Optimized double-buffered compositor with:
 *   1. Bounded Dirty Rectangles (Partial screen updates)
 *   2. Lightweight Cursor Save/Restore path (0% CPU mouse moves)
 *   3. Inline pixel raw drawing to bypass tracking inside loops
 */

#pragma GCC optimize("O3")
#pragma GCC target("sse2")

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
static uint32_t *g_wallpaper_cache = NULL;

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

void comp_set_dirty_rect(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= (int32_t)g_comp.fb_width) x2 = (int32_t)g_comp.fb_width - 1;
    if (y2 >= (int32_t)g_comp.fb_height) y2 = (int32_t)g_comp.fb_height - 1;

    if (x1 <= x2 && y1 <= y2) {
        g_dirty_x1 = x1;
        g_dirty_y1 = y1;
        g_dirty_x2 = x2;
        g_dirty_y2 = y2;
        g_comp.dirty = true;
    }
}

void comp_get_cursor_save_info(int32_t *x, int32_t *y, bool *has_cursor)
{
    if (x) *x = saved_cursor_x;
    if (y) *y = saved_cursor_y;
    if (has_cursor) *has_cursor = has_saved_cursor;
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

static void comp_precompute_wallpaper(void)
{
    uint32_t w = g_comp.fb_width;
    uint32_t h = g_comp.fb_height;
    uint32_t buf_size = w * h * 4;

    g_wallpaper_cache = (uint32_t *)kmalloc(buf_size);
    if (!g_wallpaper_cache) {
        serial_write_string("Compositor: ERROR - failed to allocate wallpaper cache!\r\n");
        return;
    }

    /* Nord gradient: NORD1 (#3B4252) at top → deep dark Nord at bottom */
    uint32_t top_r = 0x3B, top_g = 0x42, top_b = 0x52;
    uint32_t bot_r = 0x1F, bot_g = 0x23, bot_b = 0x2A;

    for (uint32_t y = 0; y < h; y++) {
        /* Prevent unsigned underflow by subtracting from top value */
        uint32_t r = top_r - (top_r - bot_r) * y / h;
        uint32_t g = top_g - (top_g - bot_g) * y / h;
        uint32_t b = top_b - (top_b - bot_b) * y / h;

        uint32_t *row = &g_wallpaper_cache[y * w];
        for (uint32_t x = 0; x < w; x++) {
            /* Add radial vignette */
            int32_t cx = (int32_t)x - (int32_t)(w / 2);
            int32_t cy = (int32_t)y - (int32_t)(h / 2);
            int32_t dist_sq = cx * cx + cy * cy;
            int32_t max_dist = (int32_t)((w/2) * (w/2) + (h/2) * (h/2));

            /* Darken edges by up to 15% */
            int32_t darken = (dist_sq * 40) / max_dist;
            if (darken > 40) darken = 40;

            /* 2x2 ordered dither to prevent gradient banding */
            int32_t dither = 0;
            uint32_t dx = x & 1;
            uint32_t dy = y & 1;
            if (dx == 0 && dy == 0) dither = -1;
            else if (dx == 1 && dy == 0) dither = 1;
            else if (dx == 0 && dy == 1) dither = 2;
            else dither = 0;

            int32_t pr = (int32_t)r - darken + dither;
            int32_t pg = (int32_t)g - darken + dither;
            int32_t pb = (int32_t)b - darken + dither;
            if (pr < 0) pr = 0; else if (pr > 255) pr = 255;
            if (pg < 0) pg = 0; else if (pg > 255) pg = 255;
            if (pb < 0) pb = 0; else if (pb > 255) pb = 255;

            row[x] = ((uint32_t)pr << 16) | ((uint32_t)pg << 8) | (uint32_t)pb;
        }
    }
}

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

    /* Precompute wallpaper cache */
    comp_precompute_wallpaper();

    /* Clear back-buffer to desktop background color or cached wallpaper */
    uint32_t pixel_count = g_comp.fb_width * g_comp.fb_height;
    if (g_wallpaper_cache) {
        memcpy(g_comp.back_buffer, g_wallpaper_cache, pixel_count * 4);
    } else {
        uint32_t bg = THEME_DESKTOP_TOP & 0x00FFFFFF;  /* strip alpha for FB */
        for (uint32_t i = 0; i < pixel_count; i++) {
            g_comp.back_buffer[i] = bg;
        }
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

        if (x1 == 0 && x2 == (int32_t)w - 1 && pitch_pixels == w) {
            /* Fast continuous block memory copy (speeds up full screen redraws) */
            memcpy(&g_comp.front_buffer[y1 * w],
                   &g_comp.back_buffer[y1 * w],
                   (y2 - y1 + 1) * w * 4);
        } else {
            for (int32_t y = y1; y <= y2; y++) {
                memcpy(&g_comp.front_buffer[y * pitch_pixels + x1],
                       &g_comp.back_buffer[y * w + x1],
                       copy_width_bytes);
            }
        }

        /* If VirtIO-GPU driver is active, notify the host */
        extern bool virtio_gpu_is_active(void);
        extern void virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
        if (virtio_gpu_is_active()) {
            virtio_gpu_flush(x1, y1, x2 - x1 + 1, y2 - y1 + 1);
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
        uint64_t count = w;
        __asm__ volatile("rep stosl" 
                         : "+D"(dst), "+c"(count) 
                         : "a"(color) 
                         : "memory");
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
    if (alpha == 0) return;
    if (alpha == 255) {
        comp_fill_rect(x, y, w, h, argb & 0x00FFFFFF);
        return;
    }

    uint32_t src_r = (argb >> 16) & 0xFF;
    uint32_t src_g = (argb >>  8) & 0xFF;
    uint32_t src_b = (argb      ) & 0xFF;

    uint32_t inv_alpha = 255 - alpha;

    for (uint32_t row = 0; row < h; row++) {
        uint32_t *__restrict dst = &g_comp.back_buffer[(y + row) * bw + x];
        for (uint32_t col = 0; col < w; col++) {
            uint32_t bg = dst[col];
            uint32_t bg_r = (bg >> 16) & 0xFF;
            uint32_t bg_g = (bg >>  8) & 0xFF;
            uint32_t bg_b = (bg      ) & 0xFF;

            uint32_t r_sum = src_r * alpha + bg_r * inv_alpha;
            uint32_t g_sum = src_g * alpha + bg_g * inv_alpha;
            uint32_t b_sum = src_b * alpha + bg_b * inv_alpha;

            uint32_t out_r = (r_sum + 1 + (r_sum >> 8)) >> 8;
            uint32_t out_g = (g_sum + 1 + (g_sum >> 8)) >> 8;
            uint32_t out_b = (b_sum + 1 + (b_sum >> 8)) >> 8;

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
    if (w == 0 || h == 0) return;
    if (radius == 0) return;

    /* Separable sliding-window fast box blur */
    uint32_t *temp = (uint32_t *)kmalloc(w * h * sizeof(uint32_t));
    if (!temp) return;

    int32_t div = 2 * radius + 1;

    /* Pass 1: Horizontal Blur */
    for (uint32_t row = 0; row < h; row++) {
        int32_t r_sum = 0, g_sum = 0, b_sum = 0;

        /* Initialize window sum for col = 0 */
        for (int32_t kx = -(int32_t)radius; kx <= (int32_t)radius; kx++) {
            int32_t col_idx = kx;
            if (col_idx < 0) col_idx = 0;
            if (col_idx >= (int32_t)w) col_idx = w - 1;

            uint32_t pixel = buf[(y + row) * bw + (x + col_idx)];
            r_sum += (pixel >> 16) & 0xFF;
            g_sum += (pixel >> 8) & 0xFF;
            b_sum += pixel & 0xFF;
        }

        for (uint32_t col = 0; col < w; col++) {
            temp[row * w + col] = ((r_sum / div) << 16) | ((g_sum / div) << 8) | (b_sum / div);

            int32_t out_idx = (int32_t)col - radius;
            if (out_idx < 0) out_idx = 0;
            if (out_idx >= (int32_t)w) out_idx = w - 1;

            int32_t in_idx = (int32_t)col + 1 + radius;
            if (in_idx < 0) in_idx = 0;
            if (in_idx >= (int32_t)w) in_idx = w - 1;

            uint32_t pixel_out = buf[(y + row) * bw + (x + out_idx)];
            uint32_t pixel_in  = buf[(y + row) * bw + (x + in_idx)];

            r_sum = r_sum - ((pixel_out >> 16) & 0xFF) + ((pixel_in >> 16) & 0xFF);
            g_sum = g_sum - ((pixel_out >> 8) & 0xFF) + ((pixel_in >> 8) & 0xFF);
            b_sum = b_sum - (pixel_out & 0xFF) + (pixel_in & 0xFF);
        }
    }

    /* Pass 2: Vertical Blur */
    for (uint32_t col = 0; col < w; col++) {
        int32_t r_sum = 0, g_sum = 0, b_sum = 0;

        /* Initialize window sum for row = 0 */
        for (int32_t ky = -(int32_t)radius; ky <= (int32_t)radius; ky++) {
            int32_t row_idx = ky;
            if (row_idx < 0) row_idx = 0;
            if (row_idx >= (int32_t)h) row_idx = h - 1;

            uint32_t pixel = temp[row_idx * w + col];
            r_sum += (pixel >> 16) & 0xFF;
            g_sum += (pixel >> 8) & 0xFF;
            b_sum += pixel & 0xFF;
        }

        for (uint32_t row = 0; row < h; row++) {
            buf[(y + row) * bw + (x + col)] = ((r_sum / div) << 16) | ((g_sum / div) << 8) | (b_sum / div);

            int32_t out_idx = (int32_t)row - radius;
            if (out_idx < 0) out_idx = 0;
            if (out_idx >= (int32_t)h) out_idx = h - 1;

            int32_t in_idx = (int32_t)row + 1 + radius;
            if (in_idx < 0) in_idx = 0;
            if (in_idx >= (int32_t)h) in_idx = h - 1;

            uint32_t pixel_out = temp[out_idx * w + col];
            uint32_t pixel_in  = temp[in_idx * w + col];

            r_sum = r_sum - ((pixel_out >> 16) & 0xFF) + ((pixel_in >> 16) & 0xFF);
            g_sum = g_sum - ((pixel_out >> 8) & 0xFF) + ((pixel_in >> 8) & 0xFF);
            b_sum = b_sum - (pixel_out & 0xFF) + (pixel_in & 0xFF);
        }
    }

    kfree(temp);
    comp_mark_area_dirty(x, y, x + w - 1, y + h - 1);
}

static void get_braille_glyph(uint8_t mask, uint8_t *glyph)
{
    for (int i = 0; i < 16; i++) {
        glyph[i] = 0;
    }
    /*
       Dot 1 (bit 0): Col 1, Row 1 (glyph rows 2, 3)
       Dot 2 (bit 1): Col 1, Row 2 (glyph rows 6, 7)
       Dot 3 (bit 2): Col 1, Row 3 (glyph rows 10, 11)
       Dot 4 (bit 3): Col 2, Row 1 (glyph rows 2, 3)
       Dot 5 (bit 4): Col 2, Row 2 (glyph rows 6, 7)
       Dot 6 (bit 5): Col 2, Row 3 (glyph rows 10, 11)
       Dot 7 (bit 6): Col 1, Row 4 (glyph rows 14, 15)
       Dot 8 (bit 7): Col 2, Row 4 (glyph rows 14, 15)

       We'll place Braille Col 1 at glyph column 3 (bit 5, 0x20) and Col 2 at glyph column 6 (bit 2, 0x04).
    */
    if (mask & 0x01) { glyph[2] |= 0x20; glyph[3] |= 0x20; }
    if (mask & 0x02) { glyph[6] |= 0x20; glyph[7] |= 0x20; }
    if (mask & 0x04) { glyph[10] |= 0x20; glyph[11] |= 0x20; }
    if (mask & 0x08) { glyph[2] |= 0x04; glyph[3] |= 0x04; }
    if (mask & 0x10) { glyph[6] |= 0x04; glyph[7] |= 0x04; }
    if (mask & 0x20) { glyph[10] |= 0x04; glyph[11] |= 0x04; }
    if (mask & 0x40) { glyph[14] |= 0x20; glyph[15] |= 0x20; }
    if (mask & 0x80) { glyph[14] |= 0x04; glyph[15] |= 0x04; }
}

void comp_draw_char(uint32_t x, uint32_t y, uint16_t c, uint32_t fg, uint32_t bg)
{
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

void comp_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color)
{
    int32_t xmin = x0 < x1 ? x0 : x1;
    int32_t xmax = x0 > x1 ? x0 : x1;
    int32_t ymin = y0 < y1 ? y0 : y1;
    int32_t ymax = y0 > y1 ? y0 : y1;

    int32_t dx = x1 - x0;
    if (dx < 0) dx = -dx;
    int32_t dy = y1 - y0;
    if (dy < 0) dy = -dy;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx - dy;

    while (1) {
        comp_draw_pixel_raw(x0, y0, color & 0x00FFFFFF);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
    comp_mark_area_dirty(xmin, ymin, xmax, ymax);
}

void comp_draw_icon_terminal(int32_t cx, int32_t cy, int32_t r)
{
    /* Background: Nord dark grey rounded rect */
    comp_draw_rounded_rect(cx - r, cy - r, 2 * r, 2 * r, r / 4, 0xFF2E3440);

    /* Draw prompt '>' in green (0xFFA3BE8C) */
    int32_t px0 = cx - (r * 4) / 10;
    int32_t py0 = cy - (r * 3) / 10;
    int32_t px1 = cx - (r * 15) / 100;
    int32_t py1 = cy;
    int32_t py2 = cy + (r * 3) / 10;
    comp_draw_line(px0, py0, px1, py1, 0xFFA3BE8C);
    comp_draw_line(px1, py1, px0, py2, 0xFFA3BE8C);

    /* Draw cursor '_' in white (0xFFD8DEE9) */
    int32_t cx0 = cx + (r * 5) / 100;
    int32_t cx1 = cx + (r * 4) / 10;
    int32_t cy_line = cy + (r * 3) / 10;
    comp_draw_line(cx0, cy_line, cx1, cy_line, 0xFFD8DEE9);
}

void comp_draw_icon_settings(int32_t cx, int32_t cy, int32_t r)
{
    /* Purple color for Settings: NORD15 (0xFFB48EAD) */
    uint32_t gear_c = 0xFFB48EAD;
    uint32_t bg_c = THEME_DOCK_BG & 0x00FFFFFF;

    for (int32_t dy = -r; dy <= r; dy++) {
        for (int32_t dx = -r; dx <= r; dx++) {
            int32_t dist_sq = dx * dx + dy * dy;
            if (dist_sq <= r * r) {
                if (dist_sq < (r / 3) * (r / 3)) {
                    comp_draw_pixel_raw(cx + dx, cy + dy, bg_c);
                }
                else if (dist_sq <= ((r * 75) / 100) * ((r * 75) / 100)) {
                    comp_draw_pixel_raw(cx + dx, cy + dy, gear_c & 0x00FFFFFF);
                }
                else {
                    int32_t abs_dx = dx < 0 ? -dx : dx;
                    int32_t abs_dy = dy < 0 ? -dy : dy;

                    int32_t spoke_width = r / 6;
                    if (spoke_width < 2) spoke_width = 2;

                    int32_t diag_width = r / 4;
                    if (diag_width < 3) diag_width = 3;

                    bool on_spoke = false;
                    if (abs_dy <= spoke_width || abs_dx <= spoke_width) {
                        on_spoke = true;
                    }
                    else if ((dx - dy < diag_width && dx - dy > -diag_width) ||
                             (dx + dy < diag_width && dx + dy > -diag_width)) {
                        on_spoke = true;
                    }

                    if (on_spoke) {
                        comp_draw_pixel_raw(cx + dx, cy + dy, gear_c & 0x00FFFFFF);
                    }
                }
            }
        }
    }
    comp_mark_area_dirty(cx - r, cy - r, cx + r, cy + r);
}

void comp_draw_icon_browser(int32_t cx, int32_t cy, int32_t r)
{
    uint32_t ring_c = 0xFF5E81AC;
    uint32_t face_c = 0xFFECEFF4;
    uint32_t needle_red = 0xFFBF616A;
    uint32_t needle_blue = 0xFF81A1C1;
    uint32_t pin_c = 0xFFEBCB8B;

    int32_t needle_len = (r * 75) / 100;
    int32_t needle_thick = r / 5;
    if (needle_thick < 2) needle_thick = 2;

    for (int32_t dy = -r; dy <= r; dy++) {
        for (int32_t dx = -r; dx <= r; dx++) {
            int32_t dist_sq = dx * dx + dy * dy;
            if (dist_sq <= r * r) {
                if (dist_sq >= (r - 2) * (r - 2)) {
                    comp_draw_pixel_raw(cx + dx, cy + dy, ring_c & 0x00FFFFFF);
                } else {
                    int32_t u = (dx - dy);
                    int32_t v = (dx + dy);
                    int32_t max_u = (needle_len * 141) / 100;
                    
                    bool on_needle = false;
                    uint32_t needle_color = 0;

                    if (u >= 0 && u <= max_u) {
                        int32_t limit_v_exact = needle_thick - (u * needle_thick) / max_u;
                        if (v >= -limit_v_exact && v <= limit_v_exact) {
                            on_needle = true;
                            needle_color = needle_red & 0x00FFFFFF;
                        }
                    } else if (u < 0 && u >= -max_u) {
                        int32_t limit_v_exact = needle_thick - ((-u) * needle_thick) / max_u;
                        if (v >= -limit_v_exact && v <= limit_v_exact) {
                            on_needle = true;
                            needle_color = needle_blue & 0x00FFFFFF;
                        }
                    }

                    if (on_needle) {
                        if (dist_sq <= (r / 8) * (r / 8)) {
                            comp_draw_pixel_raw(cx + dx, cy + dy, pin_c & 0x00FFFFFF);
                        } else {
                            comp_draw_pixel_raw(cx + dx, cy + dy, needle_color);
                        }
                    } else {
                        if (dist_sq <= (r / 8) * (r / 8)) {
                            comp_draw_pixel_raw(cx + dx, cy + dy, pin_c & 0x00FFFFFF);
                        } else {
                            comp_draw_pixel_raw(cx + dx, cy + dy, face_c & 0x00FFFFFF);
                        }
                    }
                }
            }
        }
    }
    comp_mark_area_dirty(cx - r, cy - r, cx + r, cy + r);
}

void comp_draw_icon_forge(int32_t cx, int32_t cy, int32_t r)
{
    /* Background: Nord dark grey rounded rect */
    comp_draw_rounded_rect(cx - r, cy - r, 2 * r, 2 * r, r / 4, 0xFF2E3440);

    /* Draw a folder icon in Nord8 (Teal/Blue: 0xFF88C0D0) */
    uint32_t folder_color = 0xFF88C0D0; /* Nord8 */
    uint32_t shadow_color = 0xFF3B4252; /* Nord1 */
    uint32_t tab_color = 0xFF81A1C1;    /* Nord9 */

    /* Folder dimensions scaled with r */
    int32_t fw = (r * 13) / 10;
    int32_t fh = (r * 10) / 10;
    int32_t fx = cx - fw / 2;
    int32_t fy = cy - fh / 2 + 2;

    /* Draw back tab */
    comp_fill_rect(fx + 2, fy - 2, (fw * 4) / 10, 3, tab_color & 0x00FFFFFF);

    /* Draw main body shadow */
    comp_fill_rect(fx + 1, fy + 1, fw, fh, shadow_color & 0x00FFFFFF);

    /* Draw main folder body */
    comp_draw_rounded_rect(fx, fy, fw, fh, 3, folder_color & 0x00FFFFFF);

    /* Draw front pocket flap line */
    comp_draw_line(fx, fy + 4, fx + fw - 1, fy + 4, tab_color & 0x00FFFFFF);

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
        uint32_t r_sum = src_r * alpha + bg_r * inv_alpha;
        uint32_t g_sum = src_g * alpha + bg_g * inv_alpha;
        uint32_t b_sum = src_b * alpha + bg_b * inv_alpha;
        
        /* Division-free bitwise approximation of '/ 255' */
        uint32_t out_r = (r_sum + 1 + (r_sum >> 8)) >> 8;
        uint32_t out_g = (g_sum + 1 + (g_sum >> 8)) >> 8;
        uint32_t out_b = (b_sum + 1 + (b_sum >> 8)) >> 8;
        
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

void comp_draw_rounded_rect_border(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                   uint32_t r, uint32_t color)
{
    /* Horizontal borders */
    for (uint32_t px = x + r; px < x + w - r; px++) {
        comp_draw_pixel_alpha(px, y, color);
        comp_draw_pixel_alpha(px, y + h - 1, color);
    }
    /* Vertical borders */
    for (uint32_t py = y + r; py < y + h - r; py++) {
        comp_draw_pixel_alpha(x, py, color);
        comp_draw_pixel_alpha(x + w - 1, py, color);
    }
    /* Corner arcs */
    for (int32_t dy = -(int32_t)r; dy <= 0; dy++) {
        for (int32_t dx = -(int32_t)r; dx <= 0; dx++) {
            int32_t dist_sq = dx * dx + dy * dy;
            int32_t r_inner = r - 1;
            if (dist_sq <= (int32_t)(r * r) && dist_sq > (int32_t)(r_inner * r_inner)) {
                comp_draw_pixel_alpha(x + r + dx, y + r + dy, color);
                comp_draw_pixel_alpha(x + w - 1 - r - dx, y + r + dy, color);
                comp_draw_pixel_alpha(x + r + dx, y + h - 1 - r - dy, color);
                comp_draw_pixel_alpha(x + w - 1 - r - dx, y + h - 1 - r - dy, color);
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

bool comp_is_dirty(void)
{
    return g_comp.dirty;
}

/* ============================================================
 * Desktop Wallpaper (Nord gradient)
 * ============================================================ */

void comp_draw_wallpaper(void)
{
    if (g_wallpaper_cache) {
        uint32_t w = g_comp.fb_width;
        uint32_t h = g_comp.fb_height;
        memcpy(g_comp.back_buffer, g_wallpaper_cache, w * h * 4);
        comp_mark_area_dirty(0, 0, w - 1, h - 1);
    }
}

void comp_draw_wallpaper_rect(uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh)
{
    if (!g_wallpaper_cache) return;

    uint32_t w = g_comp.fb_width;
    uint32_t h = g_comp.fb_height;

    if (rx >= w || ry >= h) return;
    if (rx + rw > w) rw = w - rx;
    if (ry + rh > h) rh = h - ry;

    for (uint32_t y = ry; y < ry + rh; y++) {
        memcpy(&g_comp.back_buffer[y * w + rx], &g_wallpaper_cache[y * w + rx], rw * 4);
    }
    comp_mark_area_dirty(rx, ry, rx + rw - 1, ry + rh - 1);
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
