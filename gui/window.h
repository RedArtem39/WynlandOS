#pragma once

#include <wynland/types.h>
#include <wynland/boot_info.h>
#include "widget.hpp"

#define MAX_TITLE_LEN 64

typedef struct {
    int32_t current;   /* scale, 0 to 256 (256 = 100%) */
    int32_t target;    /* target scale, 0 or 256 */
    int32_t velocity;  /* spring velocity */
} Spring;

/* Drawing helper forward declaration with C linkage */
extern "C" void draw_window_decorations(class Window *win);
class CWindow;

class Window : public ui::Widget {
public:
    uint32_t id;
    char title[MAX_TITLE_LEN];
    bool is_focused;
    bool is_maximized;
    bool is_visible;

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
    void (*draw_content)(Window *self);

    /* Event handlers */
    void (*handle_mouse_cb)(Window *self, int32_t mx, int32_t my, uint8_t buttons);
    void (*handle_key_cb)(Window *self, uint8_t scancode, char ascii);

    struct Window *next;
    struct Window *prev;

    Spring scale_spring;
    int32_t anim_direction; /* 1: opening, -1: closing, 0: idle */

    /* backing store for VM-drawn window content */
    uint32_t *backing_store;
    
    /* C++ Widget tree root pointer */
    void *cpp_widgets_root;
    CWindow *hypr_win;

    Window(uint32_t window_id, int32_t wx, int32_t wy, uint32_t ww, uint32_t wh, const char *window_title)
        : ui::Widget(wx, wy, ww, wh), id(window_id), is_focused(false), is_maximized(false), is_visible(false),
          normal_x(wx), normal_y(wy), normal_w(ww), normal_h(wh),
          prev_x(wx), prev_y(wy), prev_w(ww), prev_h(wh),
          draw_content(nullptr), handle_mouse_cb(nullptr), handle_key_cb(nullptr),
          next(nullptr), prev(nullptr), anim_direction(0), backing_store(nullptr), cpp_widgets_root(nullptr), hypr_win(nullptr) {
        
        uint32_t i = 0;
        while (window_title[i] && i < MAX_TITLE_LEN - 1) {
            title[i] = window_title[i];
            i++;
        }
        title[i] = '\0';

        scale_spring.current = 256;
        scale_spring.target = 256;
        scale_spring.velocity = 0;
    }

    void paint() override {
        /* Call draw_window_decorations helper */
        draw_window_decorations(this);

        /* Paint C++ widget children if any */
        paint_children();

        /* Paint backward-compatible draw_content callback if set */
        if (draw_content) {
            draw_content(this);
        }
    }

    void handle_mouse(int32_t mx, int32_t my, uint8_t buttons) override {
        /* Dispatch to children widgets */
        ui::Widget::handle_mouse(mx, my, buttons);

        /* Call legacy callback */
        if (handle_mouse_cb) {
            handle_mouse_cb(this, mx, my, buttons);
        }
    }

    void handle_key(uint8_t scancode, char ascii) override {
        /* Dispatch to children widgets */
        ui::Widget::handle_key(scancode, ascii);

        /* Call legacy callback */
        if (handle_key_cb) {
            handle_key_cb(this, scancode, ascii);
        }
    }
};
