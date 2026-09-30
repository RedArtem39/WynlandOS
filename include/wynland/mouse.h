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
int     mouse_read_queue(uint8_t *buf, int size);

/* Absolute pointer events (SYS_mouse_events): x, y and button mask after
   each PS/2 packet that changed anything. */
typedef struct { int32_t x, y; uint32_t buttons; } MouseEvent;
void mouse_events_attach(void); /* reset; first event = current state */
int  mouse_events_read(MouseEvent *buf, int max);
