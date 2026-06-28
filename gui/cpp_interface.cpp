#include "window.h"
#include "widget.hpp"
#include "widgets.hpp"
#include "qt.hpp"

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

    extern void comp_set_live_wallpaper(bool active, uint32_t style);

    static void on_wallpaper_default_clicked() {
        comp_set_live_wallpaper(false, 0);
    }

    static void on_wallpaper_aurora_clicked() {
        comp_set_live_wallpaper(true, 1);
    }

    static void on_wallpaper_snow_clicked() {
        comp_set_live_wallpaper(true, 2);
    }

    void wm_init_settings_widgets(void* settings_win_ptr) {
        Window* win = static_cast<Window*>(settings_win_ptr);

        /* Construct central Qt widget representing window client area */
        QWidget* central = new QWidget(win->x + 1, win->y + THEME_TITLEBAR_HEIGHT + 1, win->w - 2, win->h - THEME_TITLEBAR_HEIGHT - 2);

        /* Main vertical layout */
        QVBoxLayout* main_layout = new QVBoxLayout(central);

        /* 1. Header label */
        QLabel* header = new QLabel("System Configuration (Qt C++)", central);
        main_layout->addWidget(header);

        /* 2. Volume control section */
        QLabel* vol_label = new QLabel("Volume Control", central);
        main_layout->addWidget(vol_label);

        extern int g_sys_volume;
        QSlider* vol_slider = new QSlider(Qt::Horizontal, g_sys_volume, on_volume_changed, central);
        main_layout->addWidget(vol_slider);

        /* 3. Action buttons section */
        QLabel* act_label = new QLabel("Quick Actions", central);
        main_layout->addWidget(act_label);

        QWidget* button_container = new QWidget(central);
        QHBoxLayout* button_layout = new QHBoxLayout(button_container);

        QPushButton* chime_btn = new QPushButton("Play Chime", button_container);
        chime_btn->setCallback(on_chime_clicked);
        button_layout->addWidget(chime_btn);

        QPushButton* reboot_btn = new QPushButton("Reboot System", button_container);
        reboot_btn->setCallback(on_reboot_clicked);
        button_layout->addWidget(reboot_btn);

        main_layout->addWidget(button_container);

        /* 4. Desktop Wallpaper section */
        QLabel* wp_label = new QLabel("Desktop Wallpaper", central);
        main_layout->addWidget(wp_label);

        QWidget* wp_container = new QWidget(central);
        QHBoxLayout* wp_layout = new QHBoxLayout(wp_container);

        QPushButton* wp_def_btn = new QPushButton("Default", wp_container);
        wp_def_btn->setCallback(on_wallpaper_default_clicked);
        wp_layout->addWidget(wp_def_btn);

        QPushButton* wp_aur_btn = new QPushButton("Aurora", wp_container);
        wp_aur_btn->setCallback(on_wallpaper_aurora_clicked);
        wp_layout->addWidget(wp_aur_btn);

        QPushButton* wp_snow_btn = new QPushButton("Starfield", wp_container);
        wp_snow_btn->setCallback(on_wallpaper_snow_clicked);
        wp_layout->addWidget(wp_snow_btn);

        main_layout->addWidget(wp_container);

        /* Save C++ Widgets root container to window */
        win->cpp_widgets_root = central;
        win->handle_mouse_cb = wm_handle_mouse_cpp_widgets;
    }
}
