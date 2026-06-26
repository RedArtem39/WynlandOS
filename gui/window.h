/*
 * WynlandOS - Window Structure
 * ============================================================
 */
#pragma once

#include <wynland/types.h>
#include <wynland/boot_info.h>

#define MAX_TITLE_LEN 64

struct Window;

typedef struct Window {
    uint32_t id;
    int32_t x;
    int32_t y;
    uint32_t w;
    uint32_t h;
    char title[MAX_TITLE_LEN];
    bool is_visible;
    bool is_focused;

    /* Window specific custom drawing callback */
    void (*draw_content)(struct Window *self);

    /* Event handlers */
    void (*handle_mouse)(struct Window *self, int32_t mx, int32_t my, uint8_t buttons);
    void (*handle_key)(struct Window *self, uint8_t scancode, char ascii);

    struct Window *next;
    struct Window *prev;
} Window;

/* Drawing helpers */
void draw_window_decorations(Window *win);
