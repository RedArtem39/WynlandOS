#pragma once
#include "widget.hpp"

extern "C" {
#include "compositor.h"
#include "theme.h"
}

namespace ui {

/* Label Widget for displaying single-line text */
class Label : public Widget {
private:
    char m_text[64];
    uint32_t m_color;
public:
    Label(int32_t x, int32_t y, const char* text, uint32_t color = 0xFFECEFF4)
        : Widget(x, y, 0, 16), m_color(color) {
        uint32_t i = 0;
        while (text[i] && i < 63) {
            m_text[i] = text[i];
            i++;
        }
        m_text[i] = '\0';
        w = i * 9; /* Each character is 9 pixels wide */
    }
    
    void paint() override {
        int32_t ax, ay;
        get_absolute_pos(ax, ay);
        comp_draw_string(ax, ay, m_text, m_color & 0x00FFFFFF, 0);
    }
};

/* Button Widget: Nord-styled glassmorphic buttons */
class Button : public Widget {
private:
    char m_text[32];
    void (*m_on_click)();
    bool m_pressed;
public:
    Button(int32_t x, int32_t y, uint32_t w, uint32_t h, const char* text, void (*on_click)())
        : Widget(x, y, w, h), m_on_click(on_click), m_pressed(false) {
        uint32_t i = 0;
        while (text[i] && i < 31) {
            m_text[i] = text[i];
            i++;
        }
        m_text[i] = '\0';
    }

    void paint() override {
        int32_t ax, ay;
        get_absolute_pos(ax, ay);
        
        /* Button theme background */
        uint32_t bg_color = m_pressed ? NORD3 : NORD1;
        comp_draw_rounded_rect(ax, ay, w, h, 6, bg_color & 0x00FFFFFF);
        comp_draw_rounded_rect_border(ax, ay, w, h, 6, 0x40FFFFFF);
        
        /* Centered text */
        uint32_t text_len = 0;
        while (m_text[text_len]) text_len++;
        int32_t tx = ax + (int32_t)(w - text_len * 9) / 2;
        int32_t ty = ay + (int32_t)(h - 16) / 2;
        comp_draw_string(tx, ty, m_text, NORD6 & 0x00FFFFFF, 0);
    }

    void handle_mouse(int32_t mx, int32_t my, uint8_t buttons) override {
        bool is_left = (buttons & 1) != 0;
        bool old_pressed = m_pressed;
        if (is_left) {
            m_pressed = true;
        } else {
            if (m_pressed) {
                m_pressed = false;
                if (m_on_click) {
                    m_on_click();
                }
            }
        }
        if (m_pressed != old_pressed) {
            comp_mark_dirty();
        }
        
        /* Propagate to child widgets if any */
        Widget::handle_mouse(mx, my, buttons);
    }
};

/* Slider Widget: volume/brightness controls */
class Slider : public Widget {
private:
    int32_t m_val; /* 0 to 100 */
    void (*m_on_change)(int32_t val);
public:
    Slider(int32_t x, int32_t y, uint32_t w, int32_t val, void (*on_change)(int32_t))
        : Widget(x, y, w, 12), m_val(val), m_on_change(on_change) {}

    int32_t val() const { return m_val; }
    void set_val(int32_t val) { m_val = val; }

    void paint() override {
        int32_t ax, ay;
        get_absolute_pos(ax, ay);

        /* Track */
        comp_draw_rounded_rect(ax, ay, w, h, 6, NORD1 & 0x00FFFFFF);
        
        /* Fill progress */
        int32_t fill_w = (w * m_val) / 100;
        if (fill_w > 0) {
            comp_draw_rounded_rect(ax, ay, fill_w, h, 6, NORD8 & 0x00FFFFFF);
        }
    }

    void handle_mouse(int32_t mx, int32_t my, uint8_t buttons) override {
        bool is_left = (buttons & 1) != 0;
        if (is_left) {
            int32_t ax, ay;
            get_absolute_pos(ax, ay);
            int32_t offset_x = mx - ax;
            int32_t new_val = (offset_x * 100) / (int32_t)w;
            if (new_val < 0) new_val = 0;
            if (new_val > 100) new_val = 100;
            
            if (new_val != m_val) {
                m_val = new_val;
                if (m_on_change) {
                    m_on_change(m_val);
                }
                comp_mark_dirty();
            }
        }
        Widget::handle_mouse(mx, my, buttons);
    }
};

} // namespace ui
