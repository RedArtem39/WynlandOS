#include "theme.h"

extern "C" {

uint32_t g_theme_desktop_top = NORD0;
uint32_t g_theme_desktop_bot = 0xFF232831;
uint32_t g_theme_panel_bg = 0xC03B4252;
uint32_t g_theme_titlebar_bg = 0xD53B4252;
uint32_t g_theme_titlebar_fg = NORD6;
uint32_t g_theme_window_bg = 0xD52E3440;
uint32_t g_theme_window_border = NORD3;
uint32_t g_theme_text_primary = NORD6;
uint32_t g_theme_text_secondary = NORD4;
uint32_t g_theme_accent = NORD8;
uint32_t g_theme_term_bg = 0xC00F0F1A;
uint32_t g_theme_btn_close = NORD11;
uint32_t g_theme_btn_minimize = NORD13;
uint32_t g_theme_btn_maximize = NORD14;
uint32_t g_theme_dock_bg = 0xB03B4252;

bool g_dark_mode = true;

void theme_set_dark_mode(bool dark) {
    g_dark_mode = dark;
    if (dark) {
        /* Dark Mode (Nord Palette) */
        g_theme_desktop_top = NORD0;
        g_theme_desktop_bot = 0xFF232831;
        g_theme_panel_bg = 0xC03B4252;
        g_theme_titlebar_bg = 0xD53B4252;
        g_theme_titlebar_fg = NORD6;
        g_theme_window_bg = 0xD52E3440;
        g_theme_window_border = NORD3;
        g_theme_text_primary = NORD6;
        g_theme_text_secondary = NORD4;
        g_theme_accent = NORD8;
        g_theme_term_bg = 0xC00F0F1A;
        g_theme_dock_bg = 0xB03B4252;
    } else {
        /* Light Mode (Nord-based bright colors) */
        g_theme_desktop_top = 0xFFECEFF4;    /* NORD6 */
        g_theme_desktop_bot = 0xFFD8DEE9;    /* NORD4 */
        g_theme_panel_bg = 0xC0E5E9F0;       /* NORD5 with 75% alpha */
        g_theme_titlebar_bg = 0xD5ECEFF4;    /* NORD6 with 83% alpha */
        g_theme_titlebar_fg = NORD0;         /* NORD0 (dark text) */
        g_theme_window_bg = 0xD5E5E9F0;      /* NORD5 with 83% alpha */
        g_theme_window_border = NORD4;       /* NORD4 */
        g_theme_text_primary = NORD0;        /* NORD0 */
        g_theme_text_secondary = NORD3;      /* NORD3 */
        g_theme_accent = NORD10;             /* NORD10 */
        g_theme_term_bg = 0xD5ECEFF4;        /* NORD6 (light terminal background) */
        g_theme_dock_bg = 0xB0ECEFF4;        /* NORD6 with 69% alpha */
    }
    
    /* Mark compositor dirty so the entire desktop redraws in the new theme! */
    extern void comp_mark_dirty(void);
    comp_mark_dirty();
}

}
