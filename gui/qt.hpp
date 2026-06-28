#pragma once
#include "widget.hpp"
#include "widgets.hpp"

namespace Qt {
    enum Orientation {
        Horizontal,
        Vertical
    };
}

class QWidget : public ui::Widget {
public:
    QWidget(QWidget* parent = nullptr) 
        : ui::Widget(0, 0, 0, 0) {
        if (parent) {
            parent->add_child(this);
        }
    }
    
    QWidget(int32_t x, int32_t y, uint32_t w, uint32_t h, QWidget* parent = nullptr)
        : ui::Widget(x, y, w, h) {
        if (parent) {
            parent->add_child(this);
        }
    }

    void setGeometry(int32_t x, int32_t y, uint32_t w, uint32_t h) {
        set_pos(x, y);
        set_size(w, h);
    }

    void show() {
        set_visible(true);
    }

    void hide() {
        set_visible(false);
    }
    
    void paint() override {
        paint_children();
    }
};

class QLabel : public ui::Label {
public:
    QLabel(const char* text, QWidget* parent = nullptr)
        : ui::Label(0, 0, text) {
        if (parent) {
            parent->add_child(this);
        }
    }
    void setText(const char* text) {
        ui::Label::setText(text);
    }
};

class QPushButton : public ui::Button {
private:
    void (*m_click_callback)();
public:
    QPushButton(const char* text, QWidget* parent = nullptr)
        : ui::Button(0, 0, 80, 24, text, nullptr), m_click_callback(nullptr) {
        if (parent) {
            parent->add_child(this);
        }
    }
    void setText(const char* text) {
        ui::Button::setText(text);
    }

    void setCallback(void (*callback)()) {
        m_click_callback = callback;
    }

    void handle_mouse(int32_t mx, int32_t my, uint8_t buttons) override {
        bool is_left = (buttons & 1) != 0;
        extern void comp_mark_dirty(void);
        static bool s_pressed = false;
        
        int32_t ax, ay;
        get_absolute_pos(ax, ay);
        bool inside = (mx >= ax && mx < ax + (int32_t)w && my >= ay && my < ay + (int32_t)h);
        
        if (is_left && inside) {
            s_pressed = true;
        } else if (!is_left && s_pressed) {
            s_pressed = false;
            if (inside && m_click_callback) {
                m_click_callback();
            }
            comp_mark_dirty();
        }
        
        ui::Widget::handle_mouse(mx, my, buttons);
    }
};

class QSlider : public ui::Slider {
public:
    QSlider(Qt::Orientation orientation, int32_t val, void (*on_change)(int32_t), QWidget* parent = nullptr)
        : ui::Slider(0, 0, 120, val, on_change), m_orientation(orientation) {
        if (parent) {
            parent->add_child(this);
        }
    }
private:
    Qt::Orientation m_orientation;
};

class QLayout {
public:
    virtual void addWidget(ui::Widget* widget) = 0;
};

class QVBoxLayout : public QLayout {
private:
    QWidget* m_parent;
    ui::Widget* m_widgets[16];
    uint32_t m_widget_count;
    uint32_t m_margin;
    uint32_t m_spacing;
public:
    QVBoxLayout(QWidget* parent)
        : m_parent(parent), m_widget_count(0), m_margin(10), m_spacing(8) {}

    void addWidget(ui::Widget* widget) override {
        if (m_widget_count >= 16) return;
        m_widgets[m_widget_count++] = widget;
        m_parent->add_child(widget);
        update_layout();
    }
    
    void update_layout() {
        if (m_widget_count == 0) return;
        
        uint32_t parent_w = m_parent->w;
        uint32_t parent_h = m_parent->h;
        
        uint32_t total_spacing = m_spacing * (m_widget_count - 1);
        uint32_t available_h = parent_h - (m_margin * 2) - total_spacing;
        uint32_t child_h = available_h / m_widget_count;
        uint32_t child_w = parent_w - (m_margin * 2);
        
        for (uint32_t i = 0; i < m_widget_count; i++) {
            m_widgets[i]->x = m_margin;
            m_widgets[i]->y = m_margin + i * (child_h + m_spacing);
            m_widgets[i]->w = child_w;
            m_widgets[i]->h = child_h;
        }
    }
};

class QHBoxLayout : public QLayout {
private:
    QWidget* m_parent;
    ui::Widget* m_widgets[16];
    uint32_t m_widget_count;
    uint32_t m_margin;
    uint32_t m_spacing;
public:
    QHBoxLayout(QWidget* parent)
        : m_parent(parent), m_widget_count(0), m_margin(10), m_spacing(8) {}

    void addWidget(ui::Widget* widget) override {
        if (m_widget_count >= 16) return;
        m_widgets[m_widget_count++] = widget;
        m_parent->add_child(widget);
        update_layout();
    }
    
    void update_layout() {
        if (m_widget_count == 0) return;
        
        uint32_t parent_w = m_parent->w;
        uint32_t parent_h = m_parent->h;
        
        uint32_t total_spacing = m_spacing * (m_widget_count - 1);
        uint32_t available_w = parent_w - (m_margin * 2) - total_spacing;
        uint32_t child_w = available_w / m_widget_count;
        uint32_t child_h = parent_h - (m_margin * 2);
        
        for (uint32_t i = 0; i < m_widget_count; i++) {
            m_widgets[i]->x = m_margin + i * (child_w + m_spacing);
            m_widgets[i]->y = m_margin;
            m_widgets[i]->w = child_w;
            m_widgets[i]->h = child_h;
        }
    }
};
