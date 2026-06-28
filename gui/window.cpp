/*
 * WynlandOS - Window Decorations and Logic
 * ============================================================
 */
#include "window.h"
extern "C" {
#include "compositor.h"
#include "theme.h"
}

extern "C" void draw_window_decorations(Window *win)
{
    if (!win->is_visible) return;

    int32_t tx = win->x;
    int32_t ty = win->y;
    uint32_t tw = win->w;
    uint32_t th = win->h;

    /* Theme colors */
    uint32_t win_bg     = THEME_WINDOW_BG;
    uint32_t titlebar   = win->is_focused ? THEME_TITLEBAR_BG : ((NORD2 & 0x00FFFFFF) | 0xD5000000);
    uint32_t border     = THEME_WINDOW_BORDER & 0x00FFFFFF;
    uint32_t text_fg    = (win->is_focused ? THEME_TITLEBAR_FG : NORD4) & 0x00FFFFFF;

    /* Traffic lights (grayed out if not focused) */
    uint32_t close_c    = (win->is_focused ? THEME_BTN_CLOSE : NORD3) & 0x00FFFFFF;
    uint32_t min_c      = (win->is_focused ? THEME_BTN_MINIMIZE : NORD3) & 0x00FFFFFF;
    uint32_t max_c      = (win->is_focused ? THEME_BTN_MAXIMIZE : NORD3) & 0x00FFFFFF;

    /* If window is currently being dragged or resized, make it more translucent! */
    extern Window *g_dragged_window;
    extern Window *g_resizing_window;
    if (win == g_dragged_window || win == g_resizing_window) {
        win_bg = (win_bg & 0x00FFFFFF) | 0x90000000;      /* 56% alpha */
        titlebar = (titlebar & 0x00FFFFFF) | 0x90000000;  /* 56% alpha */
    }

    if (win->is_maximized) {
        /* Maximized: solid fill, no shadow, sharp corners, flat titlebar */
        comp_fill_rect(tx, ty, tw, th, win_bg & 0x00FFFFFF);
        comp_fill_rect(tx, ty, tw, THEME_TITLEBAR_HEIGHT, titlebar & 0x00FFFFFF);
    } else {
        /* Floating: rounded corners, drop shadow, glass titlebar */
        /* Window shadow (4-layer macOS Tahoe style soft drop shadow) */
        comp_draw_rounded_rect(tx - 24, ty - 24 + 12, tw + 48, th + 48, 24, 0x03000000);
        comp_draw_rounded_rect(tx - 16, ty - 16 + 8,  tw + 32, th + 32, 20, 0x06000000);
        comp_draw_rounded_rect(tx - 10, ty - 10 + 6,  tw + 20, th + 20, 16, 0x0C000000);
        comp_draw_rounded_rect(tx - 4,  ty - 4 + 3,   tw + 8,  th + 8,  10, 0x18000000);

        /* Client area body (starting below titlebar) with solid/translucent background */
        comp_fill_rect_alpha(tx, ty + THEME_TITLEBAR_HEIGHT, tw, th - THEME_TITLEBAR_HEIGHT - 8, win_bg);
        comp_fill_rect_alpha(tx + 8, ty + th - 8, tw - 16, 8, win_bg);

        /* Client area bottom corners (radius 8) */
        int32_t bl_cx = tx + 8;
        int32_t bl_cy = ty + th - 8;
        for (int32_t dy = 0; dy < 8; dy++) {
            for (int32_t dx = -8; dx < 0; dx++) {
                if (dx * dx + dy * dy <= 8 * 8) {
                    comp_draw_pixel_alpha(bl_cx + dx, bl_cy + dy, win_bg);
                }
            }
        }
        int32_t br_cx = tx + tw - 8;
        int32_t br_cy = ty + th - 8;
        for (int32_t dy = 0; dy < 8; dy++) {
            for (int32_t dx = 0; dx < 8; dx++) {
                if (dx * dx + dy * dy <= 8 * 8) {
                    comp_draw_pixel_alpha(br_cx + dx, br_cy + dy, win_bg);
                }
            }
        }

        /* 1. Blur background under titlebar first */
        comp_gaussian_blur(tx, ty, tw, THEME_TITLEBAR_HEIGHT, 20);

        /* 2. Title bar background using alpha blending */
        comp_fill_rect_alpha(tx + 8, ty, tw - 16, THEME_TITLEBAR_HEIGHT, titlebar);
        comp_fill_rect_alpha(tx, ty + 8, 8, THEME_TITLEBAR_HEIGHT - 8, titlebar);
        comp_fill_rect_alpha(tx + tw - 8, ty + 8, 8, THEME_TITLEBAR_HEIGHT - 8, titlebar);

        /* 3. Titlebar top rounded corners (radius 8) */
        int32_t tl_cx = tx + 8;
        int32_t tl_cy = ty + 8;
        for (int32_t dy = -8; dy < 0; dy++) {
            for (int32_t dx = -8; dx < 0; dx++) {
                if (dx * dx + dy * dy <= 8 * 8) {
                    comp_draw_pixel_alpha(tl_cx + dx, tl_cy + dy, titlebar);
                }
            }
        }
        int32_t tr_cx = tx + tw - 8;
        int32_t tr_cy = ty + 8;
        for (int32_t dy = -8; dy < 0; dy++) {
            for (int32_t dx = 0; dx < 8; dx++) {
                if (dx * dx + dy * dy <= 8 * 8) {
                    comp_draw_pixel_alpha(tr_cx + dx, tr_cy + dy, titlebar);
                }
            }
        }
    }

    /* Top border line under titlebar */
    comp_fill_rect(tx, ty + THEME_TITLEBAR_HEIGHT, tw, 1, border);

    /* Traffic light buttons */
    comp_draw_circle(tx + 18, ty + 14, 6, close_c);
    comp_draw_circle(tx + 38, ty + 14, 6, min_c);
    comp_draw_circle(tx + 58, ty + 14, 6, max_c);

    /* Centered title text */
    uint32_t title_len = 0;
    const char *tmp = win->title;
    while (*tmp++) title_len++;
    
    uint32_t title_sx = tx + (tw - title_len * 9) / 2;
    comp_draw_string(title_sx, ty + 6, win->title, text_fg, 0);
}
