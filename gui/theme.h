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

/* ---- Derived Theme Colors ---- */
#define THEME_DESKTOP_TOP      NORD0
#define THEME_DESKTOP_BOT      0xFF232831   /* slightly darker than NORD0  */
#define THEME_PANEL_BG         0xC03B4252   /* NORD1 with 75% alpha        */
#define THEME_PANEL_HEIGHT     24
#define THEME_TITLEBAR_BG      0xD53B4252   /* NORD1 with 83% alpha        */
#define THEME_TITLEBAR_HEIGHT  28
#define THEME_TITLEBAR_FG      NORD6
#define THEME_WINDOW_BG        0xD52E3440   /* NORD0 with 83% alpha */
#define THEME_WINDOW_BORDER    NORD3
#define THEME_TEXT_PRIMARY     NORD6
#define THEME_TEXT_SECONDARY   NORD4
#define THEME_ACCENT           NORD8
#define THEME_TERM_BG          0xC00F0F1A   /* Catppuccin dark with 75% alpha */

/* ---- Close / Minimize / Maximize (traffic light) ---- */
#define THEME_BTN_CLOSE        NORD11
#define THEME_BTN_MINIMIZE     NORD13
#define THEME_BTN_MAXIMIZE     NORD14

/* ---- Dock ---- */
#define THEME_DOCK_BG          0xB03B4252   /* NORD1 with 69% alpha */
#define THEME_DOCK_HEIGHT      48
#define THEME_DOCK_ICON_SIZE   32
