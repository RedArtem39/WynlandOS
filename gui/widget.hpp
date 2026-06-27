#pragma once
#include <wynland/types.h>

namespace ui {

class Widget {
protected:
    int32_t m_x, m_y;
    uint32_t m_w, m_h;
    Widget* m_parent;
    Widget* m_first_child;
    Widget* m_next_sibling;
    bool m_visible;

public:
    Widget(int32_t x, int32_t y, uint32_t w, uint32_t h)
        : m_x(x), m_y(y), m_w(w), m_h(h),
          m_parent(nullptr), m_first_child(nullptr), m_next_sibling(nullptr),
          m_visible(true) {}

    virtual ~Widget() {
        Widget* child = m_first_child;
        while (child) {
            Widget* next = child->m_next_sibling;
            delete child;
            child = next;
        }
    }

    int32_t x() const { return m_x; }
    int32_t y() const { return m_y; }
    uint32_t w() const { return m_w; }
    uint32_t h() const { return m_h; }
    bool visible() const { return m_visible; }

    void set_pos(int32_t x, int32_t y) { m_x = x; m_y = y; }
    void set_size(uint32_t w, uint32_t h) { m_w = w; m_h = h; }
    void set_visible(bool visible) { m_visible = visible; }

    Widget* parent() const { return m_parent; }
    Widget* first_child() const { return m_first_child; }
    Widget* next_sibling() const { return m_next_sibling; }

    void add_child(Widget* child) {
        if (!child) return;
        child->m_parent = this;
        
        /* Append to children list */
        if (!m_first_child) {
            m_first_child = child;
            child->m_next_sibling = nullptr;
        } else {
            Widget* curr = m_first_child;
            while (curr->m_next_sibling) {
                curr = curr->m_next_sibling;
            }
            curr->m_next_sibling = child;
            child->m_next_sibling = nullptr;
        }
    }

    void get_absolute_pos(int32_t& ax, int32_t& ay) const {
        ax = m_x;
        ay = m_y;
        Widget* p = m_parent;
        while (p) {
            ax += p->m_x;
            ay += p->m_y;
            p = p->m_parent;
        }
    }

    virtual void paint() = 0;

    virtual void handle_mouse(int32_t mx, int32_t my, uint8_t buttons) {
        Widget* child = m_first_child;
        while (child) {
            if (child->m_visible) {
                int32_t ax, ay;
                child->get_absolute_pos(ax, ay);
                if (mx >= ax && mx < ax + (int32_t)child->m_w &&
                    my >= ay && my < ay + (int32_t)child->m_h) {
                    child->handle_mouse(mx, my, buttons);
                }
            }
            child = child->m_next_sibling;
        }
    }

    virtual void handle_key(uint8_t scancode, char ascii) {
        Widget* child = m_first_child;
        while (child) {
            if (child->m_visible) {
                child->handle_key(scancode, ascii);
            }
            child = child->m_next_sibling;
        }
    }
};

} // namespace ui
