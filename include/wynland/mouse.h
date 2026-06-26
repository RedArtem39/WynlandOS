/*
 * WynlandOS - PS/2 Mouse Driver Header
 */
#pragma once

#include <wynland/types.h>
#include <wynland/boot_info.h>

void mouse_init(BootInfo *info);
void mouse_handle_interrupt(uint8_t data);

void mouse_hide(void);
void mouse_show(void);

/* Mouse cursor position */
int32_t mouse_get_x(void);
int32_t mouse_get_y(void);
uint8_t mouse_get_buttons(void);
