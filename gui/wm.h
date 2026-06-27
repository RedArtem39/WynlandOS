/*
 * WynlandOS - Window Manager Header
 * ============================================================
 */
#pragma once

#include "window.h"
#include <wynland/boot_info.h>

void wm_init(void);
void wm_main_loop(BootInfo *info);

void wm_register_window(Window *win);
void wm_raise_window(Window *win);
void wm_draw_desktop(void);

void wm_handle_mouse(int32_t mx, int32_t my, uint8_t buttons);
void wm_handle_key(uint8_t scancode, char ascii);

bool wm_is_gui_active(void);
bool wm_is_terminal_focused(void);
void wm_exit_gui(void);

