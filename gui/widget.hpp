#pragma once
#include <wynland/types.h>

namespace ui {

class Widget {
public:
    int32_t x, y;
    uint32_t w, h;
    Widget* parent;
    Widget* first_child;
    Widget* next_sibling;
    bool visible;

    Widget(int32_t wx, int32_t wy, uint32_t ww, uint32_t wh)
        : x(wx), y(wy), w(ww), h(wh),
          parent(nullptr), first_child(nullptr), next_sibling(nullptr),
          visible(true) {}

    virtual ~Widget() {
        Widget* child = first_child;
        while (child) {
            Widget* next = child->next_sibling;
            delete child;
            child = next;
        }
    }

    void set_pos(int32_t new_x, int32_t new_y) { x = new_x; y = new_y; }
    void set_size(uint32_t new_w, uint32_t new_h) { w = new_w; h = new_h; }
    void set_visible(bool is_visible) { visible = is_visible; }

    void add_child(Widget* child) {
        if (!child || child == this) return;
        child->parent = this;
        
        /* Check if child is already in children list */
        if (first_child == child) return;
        if (!first_child) {
            first_child = child;
            child->next_sibling = nullptr;
        } else {
            Widget* curr = first_child;
            while (curr->next_sibling) {
                if (curr->next_sibling == child) return; /* already added */
                curr = curr->next_sibling;
            }
            if (curr != child) {
                curr->next_sibling = child;
                child->next_sibling = nullptr;
            }
        }
    }

    void get_absolute_pos(int32_t& ax, int32_t& ay) const {
        ax = x;
        ay = y;
        Widget* p = parent;
        while (p) {
            ax += p->x;
            ay += p->y;
            p = p->parent;
        }
    }

    virtual void paint() = 0;

    void paint_children() {
        Widget* child = first_child;
        while (child) {
            if (child->visible) {
                child->paint();
            }
            child = child->next_sibling;
        }
    }

    virtual void handle_mouse(int32_t mx, int32_t my, uint8_t buttons) {
        Widget* child = first_child;
        while (child) {
            if (child->visible) {
                int32_t ax, ay;
                child->get_absolute_pos(ax, ay);
                if (mx >= ax && mx < ax + (int32_t)child->w &&
                    my >= ay && my < ay + (int32_t)child->h) {
                    child->handle_mouse(mx, my, buttons);
                }
            }
            child = child->next_sibling;
        }
    }

    virtual uint8_t get_hover_cursor(int32_t mx, int32_t my) {
        Widget* child = first_child;
        while (child) {
            if (child->visible) {
                int32_t ax, ay;
                child->get_absolute_pos(ax, ay);
                if (mx >= ax && mx < ax + (int32_t)child->w &&
                    my >= ay && my < ay + (int32_t)child->h) {
                    uint8_t c = child->get_hover_cursor(mx, my);
                    if (c != 0) return c;
                }
            }
            child = child->next_sibling;
        }
        return 0;
    }

    virtual void handle_key(uint8_t scancode, char ascii) {
        Widget* child = first_child;
        while (child) {
            if (child->visible) {
                child->handle_key(scancode, ascii);
            }
            child = child->next_sibling;
        }
    }
};

} // namespace ui
