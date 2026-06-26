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
    bool is_maximized;

    /* Original floating bounds saved before maximization */
    int32_t normal_x;
    int32_t normal_y;
    uint32_t normal_w;
    uint32_t normal_h;

    /* Previous bounds for optimized dirty rect rendering */
    int32_t prev_x;
    int32_t prev_y;
    uint32_t prev_w;
    uint32_t prev_h;

    /* Window specific custom drawing callback */
    void (*draw_content)(struct Window *self);

    /* Event handlers */
    void (*handle_mouse)(struct Window *self, int32_t mx, int32_t my, uint8_t buttons);
    void (*handle_key)(struct Window *self, uint8_t scancode, char ascii);

    struct Window *next;
    struct Window *prev;

    /* backing store for VM-drawn window content */
    uint32_t *backing_store;
} Window;

/* Drawing helpers */
void draw_window_decorations(Window *win);
