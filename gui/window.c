/*
 * WynlandOS - Window Decorations and Logic
 * ============================================================
 */
#include "window.h"
#include "compositor.h"
#include "theme.h"

void draw_window_decorations(Window *win)
{
    if (!win->is_visible) return;

    int32_t tx = win->x;
    int32_t ty = win->y;
    uint32_t tw = win->w;
    uint32_t th = win->h;

    /* Theme colors */
    uint32_t win_bg     = THEME_WINDOW_BG & 0x00FFFFFF;
    uint32_t titlebar   = (win->is_focused ? THEME_TITLEBAR_BG : NORD2) & 0x00FFFFFF;
    uint32_t border     = THEME_WINDOW_BORDER & 0x00FFFFFF;
    uint32_t text_fg    = (win->is_focused ? THEME_TITLEBAR_FG : NORD4) & 0x00FFFFFF;

    /* Traffic lights (grayed out if not focused) */
    uint32_t close_c    = (win->is_focused ? THEME_BTN_CLOSE : NORD3) & 0x00FFFFFF;
    uint32_t min_c      = (win->is_focused ? THEME_BTN_MINIMIZE : NORD3) & 0x00FFFFFF;
    uint32_t max_c      = (win->is_focused ? THEME_BTN_MAXIMIZE : NORD3) & 0x00FFFFFF;

    /* Window shadow (subtle dark rect behind window) */
    comp_fill_rect_alpha(tx + 4, ty + 4, tw, th, 0x40000000);

    /* Window body with rounded corners */
    comp_draw_rounded_rect(tx, ty, tw, th, 8, win_bg);

    /* Title bar (flat filled area on top portion of the window) */
    comp_fill_rect(tx + 8, ty, tw - 16, THEME_TITLEBAR_HEIGHT, titlebar);
    comp_fill_rect(tx, ty + 8, 8, THEME_TITLEBAR_HEIGHT - 8, titlebar);
    comp_fill_rect(tx + tw - 8, ty + 8, 8, THEME_TITLEBAR_HEIGHT - 8, titlebar);

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
