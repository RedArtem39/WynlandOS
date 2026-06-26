/*
 * WynlandOS - Framebuffer Compositor
 * ============================================================
 * Double-buffered compositor that manages all screen rendering.
 * Every GUI element draws into the back-buffer; the compositor
 * flips it to the hardware framebuffer once per frame.
 */
#pragma once

#include <wynland/types.h>
#include <wynland/boot_info.h>

/* ---- Compositor state ---- */
typedef struct {
    BootInfo   *boot_info;

    /* Hardware framebuffer (front) */
    uint32_t   *front_buffer;
    uint32_t    fb_width;
    uint32_t    fb_height;
    uint32_t    fb_pitch;       /* bytes per scanline */

    /* Software back-buffer */
    uint32_t   *back_buffer;

    /* Dirty flag — set to true when something changed */
    bool        dirty;

    /* Frame counter */
    uint64_t    frame_count;
} Compositor;

/* ---- Lifecycle ---- */
void compositor_init(BootInfo *info);
void compositor_flip(void);            /* copy back → front */

/* ---- Drawing primitives (draw into back-buffer) ---- */
void comp_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void comp_draw_pixel(uint32_t x, uint32_t y, uint32_t color);
void comp_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg);
void comp_draw_string(uint32_t x, uint32_t y, const char *str, uint32_t fg, uint32_t bg);
void comp_draw_circle(uint32_t cx, uint32_t cy, uint32_t r, uint32_t color);
void comp_draw_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            uint32_t r, uint32_t color);

/* ---- Alpha blending & Blur ---- */
void comp_fill_rect_alpha(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb);
void comp_box_blur(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t radius);

/* ---- Back-buffer access ---- */
uint32_t *comp_get_backbuffer(void);
uint32_t  comp_get_width(void);
uint32_t  comp_get_height(void);

/* ---- Mark dirty ---- */
void comp_mark_dirty(void);
void comp_mark_area_dirty(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
void comp_restore_cursor_back(void);
void comp_save_cursor_back(int32_t mx, int32_t my);
void comp_clear_saved_cursor(void);

/* ---- Desktop wallpaper ---- */
void comp_draw_wallpaper(void);

/* ---- Top panel ---- */
void comp_draw_panel(void);

/* ---- Cursor rendering ---- */
void comp_draw_cursor(int32_t mx, int32_t my, uint8_t buttons);
