#include "window.h"
#include "widget.hpp"
#include "widgets.hpp"

extern "C" {
    void wm_paint_cpp_widgets(void* root) {
        if (root) {
            static_cast<ui::Widget*>(root)->paint();
        }
    }

    void wm_handle_mouse_cpp_widgets(Window* self_window, int32_t mx, int32_t my, uint8_t buttons) {
        if (self_window && self_window->cpp_widgets_root) {
            static_cast<ui::Widget*>(self_window->cpp_widgets_root)->handle_mouse(mx, my, buttons);
        }
    }

    void wm_handle_key_cpp_widgets(Window* self_window, uint8_t scancode, char ascii) {
        if (self_window && self_window->cpp_widgets_root) {
            static_cast<ui::Widget*>(self_window->cpp_widgets_root)->handle_key(scancode, ascii);
        }
    }

    void wm_set_widgets_root_pos(void* root, int32_t x, int32_t y) {
        if (root) {
            static_cast<ui::Widget*>(root)->set_pos(x, y);
        }
    }

    void wm_destroy_cpp_widgets(void* root) {
        if (root) {
            delete static_cast<ui::Widget*>(root);
        }
    }

    /* Callbacks for settings widgets */
    static void on_chime_clicked() {
        extern void play_startup_chime(void);
        play_startup_chime();
    }

    static void on_reboot_clicked() {
        extern void sys_reboot(void);
        sys_reboot();
    }

    static void on_volume_changed(int32_t val) {
        extern int g_sys_volume;
        g_sys_volume = val;
    }

    /* Helper container widget that holds children */
    class Container : public ui::Widget {
    public:
        Container(int32_t x, int32_t y, uint32_t w, uint32_t h)
            : ui::Widget(x, y, w, h) {}
        void paint() override {
            paint_children();
        }
    };

    void wm_init_settings_widgets(void* settings_win_ptr) {
        Window* win = static_cast<Window*>(settings_win_ptr);

        /* Construct root container */
        Container* root = new Container(win->x + 1, win->y + THEME_TITLEBAR_HEIGHT + 1, win->w - 2, win->h - THEME_TITLEBAR_HEIGHT - 2);

        /* Add Title Label */
        root->add_child(new ui::Label(20, 20, "System Settings (C++)"));

        /* Add Volume Control section */
        root->add_child(new ui::Label(20, 55, "Volume Control"));
        extern int g_sys_volume;
        root->add_child(new ui::Slider(20, 75, 200, g_sys_volume, on_volume_changed));

        /* Add Buttons section */
        root->add_child(new ui::Label(20, 105, "Quick Actions"));
        root->add_child(new ui::Button(20, 130, 110, 28, "Play Chime", on_chime_clicked));
        root->add_child(new ui::Button(140, 130, 90, 28, "Reboot", on_reboot_clicked));

        /* Save to Window */
        win->cpp_widgets_root = root;
        win->handle_mouse_cb = wm_handle_mouse_cpp_widgets;
    }
}
