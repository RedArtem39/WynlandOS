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
#include <wynland/vfs.h>
#include <wynland/vmm.h>
#include <wynland/pmm.h>

extern void serial_write_string(const char *str);
extern void uint_to_str(uint64_t val, char *buf);
extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);

/* ---- Global compositor instance ---- */
static Compositor g_comp;
int g_sys_brightness = 100;
static uint32_t *g_wallpaper_cache = NULL;
static bool g_live_wallpaper_active = false;
static uint32_t g_live_wallpaper_style = 0;
static uint32_t *g_blur_temp = NULL;
static uint32_t  g_blur_temp_size = 0;
static uint64_t  g_backbuffer_phys = 0;

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

    uint32_t page_count = (buf_size + 4095) / 4096;
    uint64_t wall_virt = 0x92000000ULL;
    PageTable *pml4 = vmm_get_current_pml4();

    uint8_t *phys_start = (uint8_t *)pmm_alloc_contiguous(page_count);
    if (!phys_start) {
        serial_write_string("Compositor: ERROR - failed to allocate physical page for wallpaper cache Contiguous!\r\n");
        return;
    }

    for (uint32_t i = 0; i < page_count; i++) {
        uint64_t virt = wall_virt + (uint64_t)i * 4096;
        uint64_t phys = (uint64_t)(uintptr_t)phys_start + (uint64_t)i * 4096;
        vmm_map_page(pml4, virt, phys, PAGE_WRITE | PAGE_NX);
    }
    g_wallpaper_cache = (uint32_t *)(uintptr_t)wall_virt;

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
    uint32_t page_count = (buf_size + 4095) / 4096;
    uint64_t start_virt = 0x90000000ULL;
    PageTable *pml4 = vmm_get_current_pml4();

    uint8_t *fb_phys_start = (uint8_t *)pmm_alloc_contiguous(page_count);
    if (!fb_phys_start) {
        serial_write_string("Compositor: ERROR - failedContig to allocate physical page for back-buffer!\r\n");
        return;
    }
    for (uint32_t i = 0; i < page_count; i++) {
        uint64_t virt = start_virt + (uint64_t)i * 4096;
        uint64_t phys = (uint64_t)(uintptr_t)fb_phys_start + (uint64_t)i * 4096;
        vmm_map_page(pml4, virt, phys, PAGE_WRITE | PAGE_NX);
    }
    g_comp.back_buffer = (uint32_t *)(uintptr_t)start_virt;
    g_backbuffer_phys = (uint64_t)(uintptr_t)fb_phys_start;

    /* Allocate blur temp buffer to avoid allocations inside drawing loop */
    g_blur_temp_size = g_comp.fb_width * g_comp.fb_height;
    uint64_t blur_virt = 0x91000000ULL;
    uint8_t *blur_phys_start = (uint8_t *)pmm_alloc_contiguous(page_count);
    if (!blur_phys_start) {
        serial_write_string("Compositor: ERROR - failedContig to allocate physical page for blur buffer!\r\n");
        return;
    }
    for (uint32_t i = 0; i < page_count; i++) {
        uint64_t virt = blur_virt + (uint64_t)i * 4096;
        uint64_t phys = (uint64_t)(uintptr_t)blur_phys_start + (uint64_t)i * 4096;
        vmm_map_page(pml4, virt, phys, PAGE_WRITE | PAGE_NX);
    }
    g_blur_temp = (uint32_t *)(uintptr_t)blur_virt;

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

    extern bool virtio_gpu_is_active(void);
    extern void virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    if (x1 <= x2 && y1 <= y2 && virtio_gpu_is_active()) {
        /* virtio-gpu scans the back-buffer itself (it IS resource 1's
           backing memory), so copying it into the unscanned GOP buffer
           was a full wasted frame copy. Just transfer+flush the rect.
           g_sys_brightness is not applied on this path (it never was:
           the host always read back_buffer, not front_buffer). */
        virtio_gpu_flush(x1, y1, x2 - x1 + 1, y2 - y1 + 1);
    } else if (x1 <= x2 && y1 <= y2) {
        uint32_t pitch_pixels = g_comp.fb_pitch / 4;
        uint32_t w = g_comp.fb_width;
        uint32_t copy_width_bytes = (x2 - x1 + 1) * 4;

        if (g_sys_brightness >= 100) {
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
        } else {
            uint32_t factor = g_sys_brightness;
            if (factor < 10) factor = 10; // clamp min brightness to 10%
            for (int32_t y = y1; y <= y2; y++) {
                for (int32_t x = x1; x <= x2; x++) {
                    uint32_t pixel = g_comp.back_buffer[y * w + x];
                    uint32_t r = (((pixel >> 16) & 0xFF) * factor) / 100;
                    uint32_t g = (((pixel >> 8) & 0xFF) * factor) / 100;
                    uint32_t b = ((pixel & 0xFF) * factor) / 100;
                    g_comp.front_buffer[y * pitch_pixels + x] = (r << 16) | (g << 8) | b;
                }
            }
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
    if (x + w < x || x + w > bw) w = bw - x;
    if (y + h < y || y + h > bh) h = bh - y;

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
    if (x + w < x || x + w > bw) w = bw - x;
    if (y + h < y || y + h > bh) h = bh - y;

    uint32_t alpha = (argb >> 24) & 0xFF;
    if (alpha == 0) return;
    if (alpha == 255) {
        comp_fill_rect(x, y, w, h, argb & 0x00FFFFFF);
        return;
    }

    uint32_t src_r = (argb >> 16) & 0xFF;
    uint32_t src_g = (argb >>  8) & 0xFF;
    uint32_t src_b = (argb      ) & 0xFF;

    uint32_t src_r_a = src_r * alpha;
    uint32_t src_g_a = src_g * alpha;
    uint32_t src_b_a = src_b * alpha;
    uint32_t inv_alpha = 255 - alpha;

    for (uint32_t row = 0; row < h; row++) {
        uint32_t *__restrict dst = &g_comp.back_buffer[(y + row) * bw + x];
        for (uint32_t col = 0; col < w; col++) {
            uint32_t bg = dst[col];
            uint32_t bg_r = (bg >> 16) & 0xFF;
            uint32_t bg_g = (bg >>  8) & 0xFF;
            uint32_t bg_b = (bg      ) & 0xFF;

            uint32_t r_sum = src_r_a + bg_r * inv_alpha;
            uint32_t g_sum = src_g_a + bg_g * inv_alpha;
            uint32_t b_sum = src_b_a + bg_b * inv_alpha;

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
    if (x + w < x || x + w > bw) w = bw - x;
    if (y + h < y || y + h > bh) h = bh - y;
    if (w == 0 || h == 0) return;
    if (radius == 0) return;

    /* Separable sliding-window fast box blur using static buffer */
    uint32_t *temp = g_blur_temp;
    if (!temp || w * h > g_blur_temp_size) return;

    int32_t div = 2 * radius + 1;
    uint32_t inv_div = (1u << 16) / (uint32_t)div;

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
            uint32_t out_r = (uint32_t)(((uint64_t)r_sum * inv_div) >> 16);
            uint32_t out_g = (uint32_t)(((uint64_t)g_sum * inv_div) >> 16);
            uint32_t out_b = (uint32_t)(((uint64_t)b_sum * inv_div) >> 16);
            temp[row * w + col] = (out_r << 16) | (out_g << 8) | out_b;

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
            uint32_t out_r = (uint32_t)(((uint64_t)r_sum * inv_div) >> 16);
            uint32_t out_g = (uint32_t)(((uint64_t)g_sum * inv_div) >> 16);
            uint32_t out_b = (uint32_t)(((uint64_t)b_sum * inv_div) >> 16);
            buf[(y + row) * bw + (x + col)] = (out_r << 16) | (out_g << 8) | out_b;

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

    comp_mark_area_dirty(x, y, x + w - 1, y + h - 1);
}

void comp_gaussian_blur(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t sigma)
{
    /* Stacked box blur: 3 passes with radius = sigma/3 approximates Gaussian blur */
    uint32_t r = sigma / 3;
    if (r < 1) r = 1;
    comp_box_blur(x, y, w, h, r);
    comp_box_blur(x, y, w, h, r);
    comp_box_blur(x, y, w, h, r);
}

void comp_draw_glass_surface(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             uint32_t corner_radius,
                             uint32_t tint_color,
                             uint32_t blur_radius,
                             uint32_t border_color)
{
    if (w < 2 * corner_radius) corner_radius = w / 2;
    if (h < 2 * corner_radius) corner_radius = h / 2;

    /* 1. Blur the background region under the surface */
    comp_gaussian_blur(x, y, w, h, blur_radius);

    /* 2. Draw the glass tint */
    if (corner_radius == 0) {
        comp_fill_rect_alpha(x, y, w, h, tint_color);
        /* Top highlight / bottom shadow */
        comp_fill_rect_alpha(x, y, w, 1, 0x30FFFFFF);
        comp_fill_rect_alpha(x, y + h - 1, w, 1, 0x15000000);
        /* Border */
        if ((border_color >> 24) & 0xFF) {
            comp_fill_rect_alpha(x, y, w, 1, border_color);
            comp_fill_rect_alpha(x, y + h - 1, w, 1, border_color);
            comp_fill_rect_alpha(x, y + 1, 1, h - 2, border_color);
            comp_fill_rect_alpha(x + w - 1, y + 1, 1, h - 2, border_color);
        }
    } else {
        comp_draw_rounded_rect(x, y, w, h, corner_radius, tint_color);
        /* Top highlight / bottom shadow */
        if (w > 2 * corner_radius) {
            comp_fill_rect_alpha(x + corner_radius, y, w - 2 * corner_radius, 1, 0x30FFFFFF);
            comp_fill_rect_alpha(x + corner_radius, y + h - 1, w - 2 * corner_radius, 1, 0x15000000);
        }
        /* Border */
        if ((border_color >> 24) & 0xFF) {
            comp_draw_rounded_rect_border(x, y, w, h, corner_radius, border_color);
        }
    }
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

void comp_draw_char(uint32_t x, uint32_t y, uint16_t c, uint32_t fg, uint32_t bg)
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
        for (uint32_t col = 0; col < 8; col++) {
            uint32_t sum = 0;
            for (int dy = -1; dy <= 1; dy++) {
                int r = (int)row + dy;
                uint8_t bits = (r >= 0 && r < 16) ? glyph[r] : 0;
                for (int dx = -1; dx <= 1; dx++) {
                    int c = (int)col + dx;
                    uint8_t val = (c >= 0 && c < 8 && (bits & (0x80 >> c))) ? 255 : 0;
                    uint32_t weight = (dy == 0 && dx == 0) ? 4 :
                                      (dy == 0 || dx == 0) ? 2 : 1;
                    sum += val * weight;
                }
            }
            uint8_t alpha = (uint8_t)(sum / 16);
            if (alpha > 0) {
                comp_draw_pixel_alpha(x + col, y + row, ((uint32_t)alpha << 24) | (fg & 0x00FFFFFF));
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

void comp_draw_pixel_alpha(uint32_t x, uint32_t y, uint32_t argb)
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
    if (w < 2 * r) r = w / 2;
    if (h < 2 * r) r = h / 2;

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
    if (w < 2 * r) r = w / 2;
    if (h < 2 * r) r = h / 2;

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
uint64_t  comp_get_backbuffer_phys(void) { return g_backbuffer_phys; }

/* Whichever physical framebuffer memory the display is *actually* being
   scanned from right now. virtio-gpu never looks at the raw GOP/UEFI
   framebuffer (g_comp.boot_info->fb_addr) -- it only reads the back-buffer
   above, which drivers/video/virtio_gpu.c registers as its resource's
   backing memory via RESOURCE_ATTACH_BACKING. Anything (a Ring-3 process
   via /dev/fb0, a future compositor) that wants its writes to actually
   reach the screen needs to target THIS address, not the raw GOP one. */
uint64_t fb_active_phys_addr(void) {
    extern bool virtio_gpu_is_active(void);
    if (virtio_gpu_is_active()) {
        return g_backbuffer_phys;
    }
    return g_comp.boot_info->fb_addr;
}

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

static int32_t sin_approx(int32_t x)
{
    x = x % 360;
    if (x < 0) x += 360;
    if (x <= 90) return (x * 128) / 90;
    if (x <= 180) return ((180 - x) * 128) / 90;
    if (x <= 270) return -((x - 180) * 128) / 90;
    return -((360 - x) * 128) / 90;
}

static int32_t cos_approx(int32_t x)
{
    return sin_approx(x + 90);
}

static void comp_update_live_wallpaper(void)
{
    extern uint64_t timer_get_ticks(void);
    uint64_t ticks = timer_get_ticks();
    uint32_t w = g_comp.fb_width;
    uint32_t h = g_comp.fb_height;

    if (g_live_wallpaper_style == 1) {
        int32_t phase = (int32_t)(ticks / 3);
        uint32_t top_r = 0x2A + (cos_approx(phase) * 15 / 128);
        uint32_t top_g = 0x3F + (sin_approx(phase) * 20 / 128);
        uint32_t top_b = 0x55 + (cos_approx(phase + 45) * 15 / 128);

        uint32_t bot_r = 0x1A + (sin_approx(phase + 90) * 10 / 128);
        uint32_t bot_g = 0x1E + (cos_approx(phase) * 10 / 128);
        uint32_t bot_b = 0x27 + (sin_approx(phase) * 12 / 128);

        for (uint32_t y = 0; y < h; y++) {
            uint32_t r = top_r - (top_r - bot_r) * y / h;
            uint32_t g = top_g - (top_g - bot_g) * y / h;
            uint32_t b = top_b - (top_b - bot_b) * y / h;

            uint32_t *row = &g_wallpaper_cache[y * w];
            for (uint32_t x = 0; x < w; x++) {
                int32_t cx = (int32_t)x - (int32_t)(w / 2) + (cos_approx(phase / 2) * 50 / 128);
                int32_t cy = (int32_t)y - (int32_t)(h / 2) + (sin_approx(phase / 2) * 40 / 128);
                int32_t dist_sq = cx * cx + cy * cy;
                int32_t max_dist = (int32_t)((w/2) * (w/2) + (h/2) * (h/2));

                int32_t darken = (dist_sq * 30) / max_dist;
                if (darken > 35) darken = 35;

                int32_t pr = (int32_t)r - darken;
                int32_t pg = (int32_t)g - darken;
                int32_t pb = (int32_t)b - darken;

                if (pr < 0) pr = 0;
                if (pg < 0) pg = 0;
                if (pb < 0) pb = 0;

                row[x] = (pr << 16) | (pg << 8) | pb;
            }
        }
    } 
    else if (g_live_wallpaper_style == 2) {
        typedef struct {
            int32_t x, y;
            int32_t speed;
            int32_t size;
        } Star;
        static Star stars[80];
        static bool stars_init = false;
        if (!stars_init) {
            for (int i = 0; i < 80; i++) {
                stars[i].x = (ticks * (i + 7)) % w;
                stars[i].y = (ticks * (i + 13)) % h;
                stars[i].speed = 1 + (i % 3);
                stars[i].size = 1 + (i % 2);
            }
            stars_init = true;
        }

        uint32_t top_r = 0x1A, top_g = 0x1C, top_b = 0x2A;
        uint32_t bot_r = 0x0C, bot_g = 0x0D, bot_b = 0x14;
        for (uint32_t y = 0; y < h; y++) {
            uint32_t r = top_r - (top_r - bot_r) * y / h;
            uint32_t g = top_g - (top_g - bot_g) * y / h;
            uint32_t b = top_b - (top_b - bot_b) * y / h;
            uint32_t *row = &g_wallpaper_cache[y * w];
            for (uint32_t x = 0; x < w; x++) {
                row[x] = (r << 16) | (g << 8) | b;
            }
        }

        for (int i = 0; i < 80; i++) {
            stars[i].y += stars[i].speed;
            if (stars[i].y >= (int32_t)h) {
                stars[i].y = 0;
                stars[i].x = (stars[i].x + ticks) % w;
            }
            int32_t sx = stars[i].x;
            int32_t sy = stars[i].y;
            int32_t sz = stars[i].size;
            for (int dy = 0; dy < sz; dy++) {
                if (sy + dy >= (int32_t)h) continue;
                uint32_t *row = &g_wallpaper_cache[(sy + dy) * w];
                for (int dx = 0; dx < sz; dx++) {
                    if (sx + dx >= (int32_t)w) continue;
                    row[sx + dx] = 0x00FFFFFF;
                }
            }
        }
    }
}

void comp_set_live_wallpaper(bool active, uint32_t style)
{
    g_live_wallpaper_active = active;
    g_live_wallpaper_style = style;
    if (!active) {
        comp_precompute_wallpaper();
    }
    comp_mark_dirty();
}

bool comp_load_wallpaper_bmp(const char *path)
{
    VfsFile *file = vfs_open(path);
    if (!file) {
        serial_write_string("Compositor: failed to open BMP wallpaper file\n");
        return false;
    }

    uint8_t header[54];
    if (vfs_read(file, header, 54) != 54) {
        vfs_close(file);
        return false;
    }

    if (header[0] != 'B' || header[1] != 'M') {
        vfs_close(file);
        return false;
    }

    int32_t width = *(int32_t*)&header[18];
    int32_t height = *(int32_t*)&header[22];
    uint16_t bpp = *(uint16_t*)&header[28];

    uint32_t w = g_comp.fb_width;
    uint32_t h = g_comp.fb_height;
    uint32_t buf_size = w * h * 4;

    if (!g_wallpaper_cache) {
        g_wallpaper_cache = (uint32_t *)kmalloc(buf_size);
        if (!g_wallpaper_cache) {
            vfs_close(file);
            return false;
        }
    }

    g_live_wallpaper_active = false;
    g_live_wallpaper_style = 0;

    uint32_t data_offset = *(uint32_t*)&header[10];
    vfs_seek(file, data_offset, VFS_SEEK_SET);

    if (width == (int32_t)w && height == (int32_t)h) {
        for (int32_t y = (int32_t)h - 1; y >= 0; y--) {
            uint32_t *row = &g_wallpaper_cache[y * w];
            if (bpp == 24) {
                for (uint32_t x = 0; x < w; x++) {
                    uint8_t rgb[3];
                    vfs_read(file, rgb, 3);
                    row[x] = (rgb[2] << 16) | (rgb[1] << 8) | rgb[0];
                }
                uint32_t row_bytes = w * 3;
                uint32_t padding = (4 - (row_bytes % 4)) % 4;
                if (padding > 0) {
                    uint8_t dummy[4];
                    vfs_read(file, dummy, padding);
                }
            } else if (bpp == 32) {
                vfs_read(file, row, w * 4);
            }
        }
    } else {
        serial_write_string("Compositor: BMP dimensions mismatch screen size\n");
    }

    vfs_close(file);
    comp_mark_dirty();
    return true;
}

void comp_draw_wallpaper(void)
{
    if (g_live_wallpaper_active) {
        comp_update_live_wallpaper();
        comp_mark_dirty();
    }

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

    /* Draw frosted glass panel background */
    comp_draw_glass_surface(0, 0, w, panel_h, 0, THEME_PANEL_BG, 20, 0);

    /* Draw subtle bottom separator line */
    comp_fill_rect_alpha(0, panel_h - 1, w, 1, 0x40D8DEE9); // Snow storm highlight/separator
}

uint8_t g_current_cursor_type = 0; // Standard CURSOR_ARROW

static const uint8_t hand_mask[19][18] = {
    { 0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,1,2,2,2,1,0,1,1,1,0,0,0,0 },
    { 0,0,0,1,1,1,2,2,2,1,1,2,2,2,1,0,0,0 },
    { 0,0,1,2,2,2,1,2,2,2,2,2,2,2,2,1,0,0 },
    { 0,1,2,2,2,2,2,1,2,2,2,2,2,2,2,2,1,0 },
    { 0,1,2,2,2,2,2,2,1,2,2,2,2,2,2,2,1,0 },
    { 1,2,2,2,2,2,2,2,2,1,2,2,2,2,2,2,1,0 },
    { 1,2,2,2,2,2,2,2,2,2,1,2,2,2,2,2,1,0 },
    { 1,2,2,2,2,2,2,2,2,2,2,1,1,1,1,1,0,0 },
    { 1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,0,0 },
    { 0,1,2,2,2,2,2,2,2,2,2,2,2,2,2,1,0,0 },
    { 0,0,1,2,2,2,2,2,2,2,2,2,2,2,1,0,0,0 },
    { 0,0,0,1,2,2,2,2,2,2,2,2,2,1,0,0,0,0 },
    { 0,0,0,0,1,1,2,2,2,2,2,2,1,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,1,1,1,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 }
};

static const uint8_t text_mask[19][18] = {
    { 0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0 },
    { 0,0,1,2,2,2,2,2,2,2,2,2,2,2,1,0,0,0 },
    { 0,0,0,1,1,1,1,2,2,2,1,1,1,1,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,1,1,1,1,2,2,2,1,1,1,1,0,0,0,0 },
    { 0,0,1,2,2,2,2,2,2,2,2,2,2,2,1,0,0,0 },
    { 0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 }
};

static const uint8_t resize_mask[19][18] = {
    { 1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,2,2,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,2,1,0,1,2,1,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,1,0,0,0,1,2,1,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,2,1,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,1,2,1,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,1,2,1,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,1,2,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,1,2,1,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,1,2,1,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,1,2,1,0,0,1 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,1,2,1 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,2,1 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,1 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,1 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 }
};

void comp_draw_cursor(int32_t mx, int32_t my, uint8_t buttons)
{
    extern bool virtio_gpu_is_active(void);
    if (virtio_gpu_is_active()) {
        /* GPU cursor plane: position already follows the mouse IRQ; only
           the shape needs forwarding (a no-op when unchanged). */
        extern void virtio_gpu_set_cursor_shape(uint32_t shape);
        virtio_gpu_set_cursor_shape(g_current_cursor_type);
        return;
    }

    /* 1. Save background under cursor */
    comp_save_cursor_back(mx, my);

    /* 2. Draw the cursor mask onto the back-buffer */
    uint32_t bw = g_comp.fb_width;
    uint32_t bh = g_comp.fb_height;
    uint32_t *buf = g_comp.back_buffer;

    static const uint8_t default_cursor_mask[19][18] = {
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

    const uint8_t (*selected_mask)[18] = default_cursor_mask;
    if (g_current_cursor_type == 1) {
        selected_mask = hand_mask;
    } else if (g_current_cursor_type == 2) {
        selected_mask = text_mask;
    } else if (g_current_cursor_type == 3) {
        selected_mask = resize_mask;
    }

    for (int y = 0; y < 19; y++) {
        int32_t screen_y = my + y;
        if (screen_y >= (int32_t)bh) break;
        for (int x = 0; x < 18; x++) {
            int32_t screen_x = mx + x;
            if (screen_x >= (int32_t)bw) break;

            uint8_t pixel_type = selected_mask[y][x];
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

/* Anti-aliased font engine headers */
#include <wynland/font_aa_13.h>
#include <wynland/font_aa_16.h>

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
    uint32_t max_x = x;
    uint32_t start_y = y;
    
    while (*str) {
        uint32_t c = (uint8_t)*str;
        
        // UTF-8 multi-byte decoding
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
            if (cur_x > max_x) max_x = cur_x;
        }
        str++;
    }
    
    // Mark rendering area as dirty
    uint32_t h = (size == 16) ? 22 : 18;
    comp_mark_area_dirty(x, start_y - 2, max_x + 2, y + h);
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
