/*
 * WynlandOS - Forge File Explorer GUI
 * ============================================================
 */
#pragma once

#include "window.h"

void draw_forge_content(Window *self);
void handle_forge_key(Window *self, uint8_t scancode, char ascii);
void handle_forge_mouse(Window *self, int32_t mx, int32_t my, uint8_t buttons);
