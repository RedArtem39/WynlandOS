/*
 * WynlandOS - PS/2 Mouse Driver Header
 */
#pragma once

#include <wynland/types.h>
#include <wynland/boot_info.h>

void mouse_init(BootInfo *info);
void mouse_handle_interrupt(uint8_t data);
/* a USB HID mouse: dx/dy positive right/down, buttons bit0 left, bit1
   right, bit2 middle, wheel positive = up */
void mouse_inject_usb(int dx, int dy, uint8_t buttons, int wheel);

void mouse_hide(void);
void mouse_show(void);

/* Mouse cursor position */
int32_t mouse_get_x(void);
int32_t mouse_get_y(void);
uint8_t mouse_get_buttons(void);
/* The area the pointer moves in: the screen's size now (the boot
   framebuffer's, or the mode a compositor set: drivers/video/virtgpu_drm.c) */
void    mouse_set_bounds(uint32_t w, uint32_t h);
int     mouse_read_queue(uint8_t *buf, int size);

/* Absolute pointer events (SYS_mouse_events): x, y and button mask after
   each PS/2 packet that changed anything. */
typedef struct { int32_t x, y; uint32_t buttons; } MouseEvent;
/* ... and the packet's own motion: it goes on at the screen's edge and
   while the pointer is locked */
typedef struct { int32_t x, y; uint32_t buttons; int32_t dx, dy; } MouseEventEx;
void mouse_events_attach(void); /* reset; first event = current state */
int  mouse_events_read(void *buf, int max, bool ex);   /* ex: MouseEventEx records */

/* the compositor's say over the pointer (SYS_pointer_ctl) */
void mouse_pointer_lock(bool on);
void mouse_pointer_confine(bool on, int32_t x0, int32_t y0, int32_t x1, int32_t y1);
void mouse_pointer_warp(int32_t x, int32_t y);
void mouse_pointer_hide(bool hidden);
