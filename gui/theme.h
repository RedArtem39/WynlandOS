/*
 * WynlandOS - GUI Theme (Nord Palette)
 * ============================================================
 * All colors stored as 0xAARRGGBB (alpha in high byte).
 * For framebuffer output, alpha is used only in software blending.
 */
#pragma once

#include <wynland/types.h>

/* ---- Nord Polar Night (dark backgrounds) ---- */
#define NORD0   0xFF2E3440
#define NORD1   0xFF3B4252
#define NORD2   0xFF434C5E
#define NORD3   0xFF4C566A

/* ---- Nord Snow Storm (light text / highlights) ---- */
#define NORD4   0xFFD8DEE9
#define NORD5   0xFFE5E9F0
#define NORD6   0xFFECEFF4

/* ---- Nord Frost (accent blues / teals) ---- */
#define NORD7   0xFF8FBCBB
#define NORD8   0xFF88C0D0
#define NORD9   0xFF81A1C1
#define NORD10  0xFF5E81AC

/* ---- Nord Aurora (semantic colors) ---- */
#define NORD11  0xFFBF616A   /* Red    - close button    */
#define NORD12  0xFFD08770   /* Orange                    */
#define NORD13  0xFFEBCB8B   /* Yellow - minimize button  */
#define NORD14  0xFFA3BE8C   /* Green  - maximize button  */
#define NORD15  0xFFB48EAD   /* Purple                    */

#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t g_theme_desktop_top;
extern uint32_t g_theme_desktop_bot;
extern uint32_t g_theme_panel_bg;
extern uint32_t g_theme_titlebar_bg;
extern uint32_t g_theme_titlebar_fg;
extern uint32_t g_theme_window_bg;
extern uint32_t g_theme_window_border;
extern uint32_t g_theme_text_primary;
extern uint32_t g_theme_text_secondary;
extern uint32_t g_theme_accent;
extern uint32_t g_theme_term_bg;
extern uint32_t g_theme_btn_close;
extern uint32_t g_theme_btn_minimize;
extern uint32_t g_theme_btn_maximize;
extern uint32_t g_theme_dock_bg;
extern bool g_dark_mode;
void theme_set_dark_mode(bool dark);

#ifdef __cplusplus
}
#endif

/* ---- Derived Theme Colors ---- */
#define THEME_DESKTOP_TOP      g_theme_desktop_top
#define THEME_DESKTOP_BOT      g_theme_desktop_bot
#define THEME_PANEL_BG         g_theme_panel_bg
#define THEME_PANEL_HEIGHT     24
#define THEME_TITLEBAR_BG      g_theme_titlebar_bg
#define THEME_TITLEBAR_HEIGHT  28
#define THEME_TITLEBAR_FG      g_theme_titlebar_fg
#define THEME_WINDOW_BG        g_theme_window_bg
#define THEME_WINDOW_BORDER    g_theme_window_border
#define THEME_TEXT_PRIMARY     g_theme_text_primary
#define THEME_TEXT_SECONDARY   g_theme_text_secondary
#define THEME_ACCENT           g_theme_accent
#define THEME_TERM_BG          g_theme_term_bg

/* ---- Close / Minimize / Maximize (traffic light) ---- */
#define THEME_BTN_CLOSE        g_theme_btn_close
#define THEME_BTN_MINIMIZE     g_theme_btn_minimize
#define THEME_BTN_MAXIMIZE     g_theme_btn_maximize

/* ---- Dock ---- */
#define THEME_DOCK_BG          g_theme_dock_bg
#define THEME_DOCK_HEIGHT      48
#define THEME_DOCK_ICON_SIZE   32
