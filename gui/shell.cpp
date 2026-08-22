#include "window.h"
#include "qt.hpp"

bool g_applauncher_visible = false;

#define DOCK_ICON_COUNT 6

extern "C" {
#include "compositor.h"
#include "theme.h"

typedef struct {
    char name[16];
    uint32_t color;
    int32_t cx, cy;
    uint32_t base_radius;
    uint32_t current_radius;
} DockIcon;

extern DockIcon g_dock_icons[DOCK_ICON_COUNT];
extern bool g_control_panel_visible;
extern bool g_notification_panel_visible;
extern int g_sys_brightness;

extern Window *g_windows_head;
extern Window *g_term_window;
extern Window *g_settings_window;
extern Window *g_browser_window;
extern Window *g_monitor_window;
extern Window *g_forge_window;

void wm_show_window(Window *win);
void draw_dock_icon(int i, int32_t cx, int32_t cy, int32_t r);
void wm_str_cat(char *dest, const char *src);

void uint_to_str(uint64_t val, char *buf);
size_t heap_get_used_memory(void);
size_t heap_get_free_memory(void);
uint64_t timer_get_ticks(void);
uint64_t pmm_get_free_memory(void);
uint64_t pmm_get_total_memory(void);

int mouse_get_x(void);
int mouse_get_y(void);
double sqrt_double(double val);

void play_startup_chime(void);
void beep(uint32_t freq, uint32_t duration_ms);
void nosound(void);
void sys_poweroff(void);
void sys_reboot(void);
}

/* QStatusPanel Class representing the top panel */
class QStatusPanel : public QWidget {
public:
    QStatusPanel(int32_t x, int32_t y, uint32_t w, uint32_t h)
        : QWidget(x, y, w, h) {}

    void paint() override {
        if (true) {
            /* 1. Restore wallpaper under the top bar area (0..56px) */
            extern void comp_draw_wallpaper_rect(uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh);
            comp_draw_wallpaper_rect(0, 0, w, 56);

            uint32_t bar_y = 8;
            uint32_t bar_h = 42;
            uint32_t bar_x = 6;

            /* 2. Left Pill: Help (?), Search/AppLauncher (>_), Settings (*), Workspaces (1..8) */
            uint32_t left_pill_w = 410;
            comp_draw_glass_surface(bar_x, bar_y, left_pill_w, bar_h, 14, 0xC01E1E2E, 0, 0x14CDD6F4);

            comp_draw_string_aa(bar_x + 16, bar_y + 14, "?", 0xFFA6ADC8, 14);
            comp_draw_string_aa(bar_x + 50, bar_y + 14, ">_", 0xFF89B4FA, 14);
            comp_draw_string_aa(bar_x + 88, bar_y + 14, "*", 0xFF89B4FA, 14);

            for (int i = 1; i <= 8; i++) {
                int32_t ws_x = bar_x + 124 + (i - 1) * 34;
                char ws_num[4] = { (char)('0' + i), '\0' };
                if (i == 1) {
                    comp_draw_glass_surface(ws_x, bar_y + 5, 28, 32, 10, 0xC089B4FA, 0, 0x30FFFFFF);
                    comp_draw_string_aa(ws_x + 10, bar_y + 14, ws_num, 0xFF1E1E2E, 14);
                } else if (false) {
                    comp_draw_glass_surface(ws_x, bar_y + 5, 28, 32, 10, 0x80313244, 0, 0x14CDD6F4);
                    comp_draw_string_aa(ws_x + 10, bar_y + 14, ws_num, 0xFFCDD6F4, 14);
                } else {
                    comp_draw_string_aa(ws_x + 10, bar_y + 14, ws_num, 0xFF585B70, 14);
                }
            }

            /* 3. Center Pill: Weather (21C) & Clock Time */
            uint32_t center_w = 260;
            uint32_t center_x = (w - center_w) / 2;
            comp_draw_glass_surface(center_x, bar_y, center_w, bar_h, 14, 0xC01E1E2E, 0, 0x14CDD6F4);

            extern uint64_t timer_get_ticks(void);
            uint64_t total_sec = timer_get_ticks() / 100;
            uint32_t sec = total_sec % 60;
            uint32_t min = (total_sec / 60) % 60;
            uint32_t hr  = (total_sec / 3600) % 24;

            char time_str[32];
            time_str[0] = '0' + (hr / 10);
            time_str[1] = '0' + (hr % 10);
            time_str[2] = ':';
            time_str[3] = '0' + (min / 10);
            time_str[4] = '0' + (min % 10);
            time_str[5] = ':';
            time_str[6] = '0' + (sec / 10);
            time_str[7] = '0' + (sec % 10);
            time_str[8] = '\0';

            comp_draw_string_aa(center_x + 20, bar_y + 14, "21C", 0xFFFAB387, 14);
            comp_draw_string_aa(center_x + 65, bar_y + 14, "|", 0xFF45475A, 14);
            comp_draw_string_aa(center_x + 85, bar_y + 14, time_str, 0xFFCDD6F4, 14);
            comp_draw_string_aa(center_x + 165, bar_y + 14, "Quickshell", 0xFFA6ADC8, 13);

            /* 4. Right Pill: Keyboard Layout, Network, Volume, Power */
            uint32_t right_w = 340;
            uint32_t right_x = w - 6 - right_w;
            comp_draw_glass_surface(right_x, bar_y, right_w, bar_h, 14, 0xC01E1E2E, 0, 0x14CDD6F4);

            extern bool layout_ru;
            comp_draw_glass_surface(right_x + 10, bar_y + 5, 46, 32, 10, 0x80313244, 0, 0x14CDD6F4);
            comp_draw_string_aa(right_x + 21, bar_y + 14, layout_ru ? "RU" : "EN", 0xFFCDD6F4, 14);

            comp_draw_glass_surface(right_x + 64, bar_y + 5, 110, 32, 10, 0xC089B4FA, 0, 0x30FFFFFF);
            comp_draw_string_aa(right_x + 76, bar_y + 14, "Connected", 0xFF1E1E2E, 14);

            comp_draw_glass_surface(right_x + 182, bar_y + 5, 74, 32, 10, 0x80313244, 0, 0x14CDD6F4);
            comp_draw_string_aa(right_x + 192, bar_y + 14, "Vol 85%", 0xFFCDD6F4, 14);

            comp_draw_glass_surface(right_x + 264, bar_y + 5, 62, 32, 10, 0xC0A6E3A1, 0, 0x30FFFFFF);
            comp_draw_string_aa(right_x + 280, bar_y + 14, "PWR", 0xFF1E1E2E, 14);
            return;
        }

        /* 1. Restore wallpaper under the panel to prevent alpha accumulation */
        extern void comp_draw_wallpaper_rect(uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh);
        comp_draw_wallpaper_rect(x, y, w, h);

        /* 2. Draw base panel background and border */
        extern void comp_draw_panel(void);
        comp_draw_panel();

        uint32_t fg = THEME_TEXT_PRIMARY & 0x00FFFFFF;
        uint32_t pipe_fg = NORD3 & 0x00FFFFFF;

        /* 3. Draw WynlandOS Logo & Name */
        comp_draw_string_aa(x + 10, y + 21, "W", THEME_ACCENT & 0x00FFFFFF, 13);
        comp_draw_string_aa(x + 23, y + 21, "ynlandOS", fg, 13);

        /* 4. Active window title */
        extern Window *g_windows_head;
        const char *active_title = "Desktop";
        if (g_windows_head && g_windows_head->is_visible && g_windows_head->scale_spring.target == 256) {
            active_title = g_windows_head->title;
        }
        comp_draw_string_aa(x + 115, y + 21, "|", pipe_fg, 13);
        comp_draw_string_aa(x + 130, y + 21, active_title, NORD8 & 0x00FFFFFF, 13);

        /* 5. Keyboard Layout Indicator */
        extern bool layout_ru;
        comp_draw_string_aa(x + w - 130, y + 21, "|", pipe_fg, 13);
        comp_draw_string_aa(x + w - 110, y + 21, layout_ru ? "RU" : "EN", THEME_TEXT_SECONDARY & 0x00FFFFFF, 13);

        /* 6. RAM / Heap memory stats */
        extern size_t heap_get_used_memory(void);
        extern size_t heap_get_free_memory(void);
        size_t used = heap_get_used_memory();
        size_t total = used + heap_get_free_memory();

        char mem_str[64] = "RAM: ";
        char num_buf[32];
        extern void uint_to_str(uint64_t val, char *buf);
        uint_to_str(used / 1024 / 1024, num_buf);
        extern void wm_str_cat(char *dest, const char *src);
        wm_str_cat(mem_str, num_buf);
        wm_str_cat(mem_str, "M/");
        uint_to_str(total / 1024 / 1024, num_buf);
        wm_str_cat(mem_str, num_buf);
        wm_str_cat(mem_str, "M");

        comp_draw_string_aa(x + w - 240, y + 21, mem_str, THEME_TEXT_SECONDARY & 0x00FFFFFF, 13);

        /* 7. Current Clock Time */
        extern uint64_t timer_get_ticks(void);
        uint64_t total_sec = timer_get_ticks() / 100;
        uint32_t sec = total_sec % 60;
        uint32_t min = (total_sec / 60) % 60;
        uint32_t hr  = (total_sec / 3600) % 24;

        char time_str[32];
        time_str[0] = '0' + (hr / 10);
        time_str[1] = '0' + (hr % 10);
        time_str[2] = ':';
        time_str[3] = '0' + (min / 10);
        time_str[4] = '0' + (min % 10);
        time_str[5] = ':';
        time_str[6] = '0' + (sec / 10);
        time_str[7] = '0' + (sec % 10);
        time_str[8] = '\0';

        comp_draw_string_aa(x + w - 75, y + 21, time_str, fg, 13);
    }

    void handle_mouse(int32_t mx, int32_t my, uint8_t buttons) override {
        (void)my;
        bool left_pressed  = (buttons & 1) != 0;
        static bool s_prev_left = false;
        bool clicked_down = left_pressed && !s_prev_left;
        s_prev_left = left_pressed;

        if (clicked_down) {
            uint32_t bar_x = 6;
            if (mx >= (int32_t)bar_x + 40 && mx < (int32_t)bar_x + 78) {
                /* Clicked >_ AppLauncher / Terminal */
                g_applauncher_visible = !g_applauncher_visible;
                extern void comp_mark_dirty(void);
                comp_mark_dirty();
            } else if (mx >= (int32_t)bar_x + 78 && mx < (int32_t)bar_x + 118) {
                /* Clicked * Settings */
                extern Window *g_settings_window;
                if (g_settings_window) {
                    if (g_settings_window->is_visible) {
                        g_settings_window->is_visible = false;
                    } else {
                        wm_show_window(g_settings_window);
                    }
                    extern void comp_mark_dirty(void);
                    comp_mark_dirty();
                }
            } else if (mx >= (int32_t)bar_x + 124 && mx < (int32_t)bar_x + 124 + 8 * 34) {
                /* Clicked Workspaces 1..8 */
                int ws_idx = (mx - ((int32_t)bar_x + 124)) / 34 + 1;
                if (ws_idx >= 1 && ws_idx <= 8) {
                    extern void comp_mark_dirty(void);
                    comp_mark_dirty();
                }
            } else if (mx >= (int32_t)w - 76 && mx < (int32_t)w - 6) {
                /* Clicked PWR button -> toggle Control Center */
                g_control_panel_visible = !g_control_panel_visible;
                extern void comp_mark_dirty(void);
                comp_mark_dirty();
            }
            return;
        }

        if (clicked_down) {
            if (mx >= (int32_t)w - 80) {
                /* Click clock -> toggle Notification Center */
                g_notification_panel_visible = !g_notification_panel_visible;
                g_control_panel_visible = false;
                extern void comp_mark_dirty(void);
                comp_mark_dirty();
            } else if (mx >= (int32_t)w - 160 && mx < (int32_t)w - 80) {
                /* Click tray icons -> toggle Control Center */
                g_control_panel_visible = !g_control_panel_visible;
                g_notification_panel_visible = false;
                extern void comp_mark_dirty(void);
                comp_mark_dirty();
            }
        }
    }
};

/* QDock Class representing application launcher at the bottom */
class QDock : public QWidget {
public:
    QDock(int32_t x, int32_t y, uint32_t w, uint32_t h)
        : QWidget(x, y, w, h) {}

    void paint() override {
        if (true) {
            if (!g_applauncher_visible) return;
            uint32_t sw = comp_get_width();
            uint32_t sh = comp_get_height();
            uint32_t pop_w = 480;
            uint32_t pop_h = 360;
            int32_t pop_x = (sw - pop_w) / 2;
            int32_t pop_y = (sh - pop_h) / 2;

            /* Frosted glass AppLauncher card (Catppuccin Mocha Base #1e1e2e) */
            comp_draw_glass_surface(pop_x, pop_y, pop_w, pop_h, 16, 0xE01E1E2E, 0, 0x30CDD6F4);

            comp_draw_string_aa(pop_x + 24, pop_y + 30, "Quickshell Application Launcher", 0xFFCDD6F4, 16);
            comp_draw_string_aa(pop_x + pop_w - 40, pop_y + 30, "[X]", 0xFFF38BA8, 14);

            /* Search bar */
            comp_draw_glass_surface(pop_x + 24, pop_y + 50, pop_w - 48, 38, 10, 0xC0313244, 0, 0x20CDD6F4);
            comp_draw_string_aa(pop_x + 40, pop_y + 73, "Search apps across NixOS / WynlandOS...", 0xFFA6ADC8, 14);

            /* 6 App Cards */
            const char* app_names[6] = { "Wynland Terminal", "System Settings", "NixOS Browser", "3D Forge / Engine", "System Monitor", "MiniGL Demo" };
            const char* app_icons[6] = { ">_", "*", "@", "#", "%", "+" };
            uint32_t app_colors[6] = { 0xFF89B4FA, 0xFFA6E3A1, 0xFFFAB387, 0xFFCBA6F7, 0xFF74C7EC, 0xFFF9E2AF };

            for (int i = 0; i < 6; i++) {
                int col = i % 2;
                int row = i / 2;
                int32_t card_x = pop_x + 24 + col * 220;
                int32_t card_y = pop_y + 104 + row * 76;
                comp_draw_glass_surface(card_x, card_y, 210, 64, 12, 0x80313244, 0, 0x18CDD6F4);
                comp_draw_circle(card_x + 28, card_y + 32, 16, app_colors[i] & 0x00FFFFFF);
                comp_draw_string_aa(card_x + 21, card_y + 36, app_icons[i], 0xFF1E1E2E, 14);
                comp_draw_string_aa(card_x + 56, card_y + 36, app_names[i], 0xFFCDD6F4, 14);
            }
            return;
        }

        /* Draw glass dock frame */
        comp_draw_glass_surface(x, y, w, h, 12, THEME_DOCK_BG, 20, 0x40FFFFFF);

        extern int mouse_get_x(void);
        extern int mouse_get_y(void);
        int32_t mx = mouse_get_x();
        int32_t my = mouse_get_y();

        int spacing = w / (DOCK_ICON_COUNT + 1);
        for (int i = 0; i < DOCK_ICON_COUNT; i++) {
            g_dock_icons[i].cx = x + spacing * (i + 1);
            g_dock_icons[i].cy = y + h / 2;

            /* Zoom magnification calculation */
            int32_t dx = mx - g_dock_icons[i].cx;
            int32_t dy = my - g_dock_icons[i].cy;
            if (dx * dx + dy * dy <= 48 * 48) {
                extern double sqrt_double(double val);
                int zoom = 16 - (int)(sqrt_double(dx * dx + dy * dy) / 3);
                if (zoom < 0) zoom = 0;
                g_dock_icons[i].current_radius = g_dock_icons[i].base_radius + zoom;
            } else {
                g_dock_icons[i].current_radius = g_dock_icons[i].base_radius;
            }

            extern void draw_dock_icon(int idx, int32_t cx, int32_t cy, int32_t r);
            draw_dock_icon(i, g_dock_icons[i].cx, g_dock_icons[i].cy, g_dock_icons[i].current_radius);

            /* Dot indicators for running programs */
            extern Window *g_term_window;
            extern Window *g_settings_window;
            extern Window *g_browser_window;
            extern Window *g_forge_window;
            extern Window *g_opengl_window;
            extern Window *g_monitor_window;

            bool active = false;
            if (i == 0 && g_term_window && g_term_window->is_visible) active = true;
            if (i == 1 && g_settings_window && g_settings_window->is_visible) active = true;
            if (i == 2 && g_browser_window && g_browser_window->is_visible) active = true;
            if (i == 3 && g_forge_window && g_forge_window->is_visible) active = true;
            if (i == 4 && g_opengl_window && g_opengl_window->is_visible) active = true;
            if (i == 5 && g_monitor_window && g_monitor_window->is_visible) active = true;

            if (active) {
                comp_draw_circle(g_dock_icons[i].cx, y + h - 4, 2, THEME_ACCENT & 0x00FFFFFF);
            }
        }

        /* Tooltips */
        for (int k = 0; k < DOCK_ICON_COUNT; k++) {
            int32_t dx = mx - g_dock_icons[k].cx;
            int32_t dy = my - g_dock_icons[k].cy;
            int32_t rad = (int32_t)g_dock_icons[k].current_radius;
            if (dx * dx + dy * dy <= rad * rad) {
                const char *name = g_dock_icons[k].name;
                uint32_t text_width = comp_string_width_aa(name, 13);
                uint32_t box_w = text_width + 16;
                uint32_t box_h = 20;

                int32_t box_x = g_dock_icons[k].cx - (int32_t)box_w / 2;
                int32_t box_y = g_dock_icons[k].cy - rad - 24;

                comp_draw_rounded_rect(box_x, box_y, box_w, box_h, 4, NORD0 & 0x00FFFFFF);
                comp_draw_rounded_rect_border(box_x, box_y, box_w, box_h, 4, 0x30FFFFFF);
                comp_draw_string_aa(box_x + 8, box_y + 15, name, 0xFFECEFF4, 13);
            }
        }
    }

    void handle_mouse(int32_t mx, int32_t my, uint8_t buttons) override {
        bool left_pressed  = (buttons & 1) != 0;
        static bool s_prev_left = false;
        bool clicked_down = left_pressed && !s_prev_left;
        s_prev_left = left_pressed;

        if (true) {
            if (!g_applauncher_visible || !clicked_down) return;
            uint32_t sw = comp_get_width();
            uint32_t sh = comp_get_height();
            uint32_t pop_w = 480;
            uint32_t pop_h = 360;
            int32_t pop_x = (sw - pop_w) / 2;
            int32_t pop_y = (sh - pop_h) / 2;

            if (mx >= pop_x + (int32_t)pop_w - 50 && mx <= pop_x + (int32_t)pop_w && my >= pop_y && my <= pop_y + 40) {
                g_applauncher_visible = false;
                extern void comp_mark_dirty(void);
                comp_mark_dirty();
                return;
            }

            for (int i = 0; i < 6; i++) {
                int col = i % 2;
                int row = i / 2;
                int32_t card_x = pop_x + 24 + col * 220;
                int32_t card_y = pop_y + 104 + row * 76;
                if (mx >= card_x && mx <= card_x + 210 && my >= card_y && my <= card_y + 64) {
                    extern Window *g_term_window;
                    extern Window *g_settings_window;
                    extern Window *g_browser_window;
                    extern Window *g_forge_window;
                    extern Window *g_monitor_window;
                    extern void wm_show_window(Window *win);

                    if (i == 0) wm_show_window(g_term_window);
                    else if (i == 1) wm_show_window(g_settings_window);
                    else if (i == 2) wm_show_window(g_browser_window);
                    else if (i == 3) wm_show_window(g_forge_window);
                    else if (i == 4) wm_show_window(g_monitor_window);

                    g_applauncher_visible = false;
                    extern void comp_mark_dirty(void);
                    comp_mark_dirty();
                    return;
                }
            }
            return;
        }

        if (clicked_down) {
            for (int k = 0; k < DOCK_ICON_COUNT; k++) {
                int32_t dx = mx - g_dock_icons[k].cx;
                int32_t dy = my - g_dock_icons[k].cy;
                int32_t rad = (int32_t)g_dock_icons[k].current_radius;
                if (dx * dx + dy * dy <= rad * rad) {
                    extern Window *g_term_window;
                    extern Window *g_settings_window;
                    extern Window *g_browser_window;
                    extern Window *g_forge_window;
                    extern Window *g_opengl_window;
                    extern Window *g_monitor_window;
                    extern void wm_show_window(Window *win);

                    if (k == 0) wm_show_window(g_term_window);
                    else if (k == 1) wm_show_window(g_settings_window);
                    else if (k == 2) wm_show_window(g_browser_window);
                    else if (k == 3) wm_show_window(g_forge_window);
                    else if (k == 4) wm_show_window(g_opengl_window);
                    else if (k == 5) wm_show_window(g_monitor_window);

                    extern void comp_mark_dirty(void);
                    comp_mark_dirty();
                }
            }
        }
    }
};

/* QControlCenter dialog widgets container using Qt layout managers */
class QControlCenter : public QWidget {
private:
    QLabel* heading;
    QLabel* vol_label;
    QSlider* vol_slider;
    QLabel* bright_label;
    QSlider* bright_slider;
    QWidget* row1;
    QWidget* row2;
    QPushButton* chime_btn;
    QPushButton* beep_btn;
    QPushButton* mute_btn;
    QPushButton* shutdown_btn;
    QPushButton* reboot_btn;
    QWidget* row3;
    QPushButton* theme_btn;
    int32_t m_current_y;
    int32_t m_target_y;
    int32_t m_velocity_y;
public:
    QControlCenter(int32_t x, int32_t y, uint32_t w, uint32_t h)
        : QWidget(x, y, w, h) {
        m_target_y = THEME_PANEL_HEIGHT - 390;
        m_current_y = THEME_PANEL_HEIGHT - 390;
        m_velocity_y = 0;
        this->y = m_current_y;
        
        QVBoxLayout* layout = new QVBoxLayout(this);
 
        heading = new QLabel("Control Center (Qt C++)", this);
        layout->addWidget(heading);
 
        vol_label = new QLabel("Volume Level", this);
        layout->addWidget(vol_label);
 
        extern int g_sys_volume;
        vol_slider = new QSlider(Qt::Horizontal, g_sys_volume, [](int32_t val) {
            extern int g_sys_volume;
            g_sys_volume = val;
        }, this);
        layout->addWidget(vol_slider);

        bright_label = new QLabel("Screen Brightness", this);
        layout->addWidget(bright_label);

        bright_slider = new QSlider(Qt::Horizontal, g_sys_brightness, [](int32_t val) {
            g_sys_brightness = val;
        }, this);
        layout->addWidget(bright_slider);

        /* Row 1: sound tests layout */
        row1 = new QWidget(this);
        QHBoxLayout* layout1 = new QHBoxLayout(row1);
        
        chime_btn = new QPushButton("Chime", row1);
        chime_btn->setCallback([]() {
            extern void play_startup_chime(void);
            play_startup_chime();
        });
        layout1->addWidget(chime_btn);

        beep_btn = new QPushButton("Beep", row1);
        beep_btn->setCallback([]() {
            extern void beep(uint32_t freq, uint32_t duration_ms);
            beep(800, 100);
        });
        layout1->addWidget(beep_btn);

        mute_btn = new QPushButton("Mute", row1);
        mute_btn->setCallback([]() {
            extern void nosound(void);
            nosound();
        });
        layout1->addWidget(mute_btn);

        layout->addWidget(row1);

        /* Row 2: power actions layout */
        row2 = new QWidget(this);
        QHBoxLayout* layout2 = new QHBoxLayout(row2);

        shutdown_btn = new QPushButton("Power Off", row2);
        shutdown_btn->setCallback([]() {
            extern void sys_poweroff(void);
            sys_poweroff();
        });
        layout2->addWidget(shutdown_btn);

        reboot_btn = new QPushButton("Reboot", row2);
        reboot_btn->setCallback([]() {
            extern void sys_reboot(void);
            sys_reboot();
        });
        layout2->addWidget(reboot_btn);

        layout->addWidget(row2);

        /* Row 3: Theme layout */
        row3 = new QWidget(this);
        QHBoxLayout* layout3 = new QHBoxLayout(row3);

        theme_btn = new QPushButton("Mode: Dark", row3);
        theme_btn->setCallback([]() {
            theme_set_dark_mode(!g_dark_mode);
        });
        layout3->addWidget(theme_btn);

        layout->addWidget(row3);
    }

    void update_animation() {
        int32_t target = g_control_panel_visible ? (THEME_PANEL_HEIGHT + 5) : (THEME_PANEL_HEIGHT - 390);
        int32_t displacement = m_current_y - target;
        if (displacement != 0 || m_velocity_y != 0) {
            int32_t stiffness = 45;
            int32_t damping = 12;
            int32_t force = (-stiffness * displacement - damping * m_velocity_y) / 32;
            m_velocity_y += force;
            m_current_y += m_velocity_y;

            int32_t new_disp = m_current_y - target;
            int32_t abs_disp = new_disp < 0 ? -new_disp : new_disp;
            int32_t abs_vel = m_velocity_y < 0 ? -m_velocity_y : m_velocity_y;
            if (abs_disp < 2 && abs_vel < 2) {
                m_current_y = target;
                m_velocity_y = 0;
            }
            this->y = m_current_y;
            extern void comp_mark_dirty(void);
            comp_mark_dirty();
        }
    }

    void paint() override {
        update_animation();
        if (this->y <= (int32_t)THEME_PANEL_HEIGHT - 380) {
            return;
        }

        /* Dynamically update theme button label text */
        extern bool g_dark_mode;
        if (g_dark_mode) {
            theme_btn->setText("Mode: Dark");
        } else {
            theme_btn->setText("Mode: Light");
        }

        /* Draw background glass surface */
        comp_draw_glass_surface(x, y, w, h, 12, THEME_WINDOW_BG, 15, 0x30FFFFFF);

        /* Render layout elements */
        QWidget::paint();
    }
};

/* QNotificationCenter dialog widgets container */
class QNotificationCenter : public QWidget {
private:
    QLabel* date_heading;
    QLabel* calendar_label;
    QLabel* cpu_widget;
    QLabel* memory_widget;
    int32_t m_current_x;
    int32_t m_target_x;
    int32_t m_velocity_x;
public:
    QNotificationCenter(int32_t x, int32_t y, uint32_t w, uint32_t h)
        : QWidget(x, y, w, h) {
        uint32_t sw = comp_get_width();
        m_target_x = sw + 10;
        m_current_x = sw + 10;
        m_velocity_x = 0;
        this->x = m_current_x;

        QVBoxLayout* layout = new QVBoxLayout(this);

        date_heading = new QLabel("Today (Notification Center)", this);
        layout->addWidget(date_heading);

        calendar_label = new QLabel("Wednesday, Jun 27", this);
        layout->addWidget(calendar_label);

        cpu_widget = new QLabel("CPU Load: 5%", this);
        layout->addWidget(cpu_widget);

        memory_widget = new QLabel("RAM Free: 42MB / 64MB", this);
        layout->addWidget(memory_widget);
    }

    void update_animation() {
        uint32_t sw = comp_get_width();
        int32_t target = g_notification_panel_visible ? (sw - 290) : (sw + 10);
        int32_t displacement = m_current_x - target;
        if (displacement != 0 || m_velocity_x != 0) {
            int32_t stiffness = 45;
            int32_t damping = 12;
            int32_t force = (-stiffness * displacement - damping * m_velocity_x) / 32;
            m_velocity_x += force;
            m_current_x += m_velocity_x;

            int32_t new_disp = m_current_x - target;
            int32_t abs_disp = new_disp < 0 ? -new_disp : new_disp;
            int32_t abs_vel = m_velocity_x < 0 ? -m_velocity_x : m_velocity_x;
            if (abs_disp < 2 && abs_vel < 2) {
                m_current_x = target;
                m_velocity_x = 0;
            }
            this->x = m_current_x;
            extern void comp_mark_dirty(void);
            comp_mark_dirty();
        }
    }

    void paint() override {
        update_animation();
        uint32_t sw = comp_get_width();
        if (this->x >= (int32_t)sw) {
            return;
        }

        /* 1. Dynamic CPU Load Mockup (correlated with drag activities) */
        extern Window *g_dragged_window, *g_resizing_window;
        uint32_t cpu_base = (g_dragged_window || g_resizing_window) ? 14 : 3;
        uint64_t ticks = timer_get_ticks();
        uint32_t cpu_load = cpu_base + (ticks % 5); // fluctuates between +0..4%
        
        char cpu_str[64] = "CPU Load: ";
        char num_buf[32];
        uint_to_str(cpu_load, num_buf);
        wm_str_cat(cpu_str, num_buf);
        wm_str_cat(cpu_str, "%");
        cpu_widget->setText(cpu_str);

        /* 2. Dynamic Memory Widget from real physical memory manager */
        size_t free_mem = (size_t)pmm_get_free_memory();
        size_t total = (size_t)pmm_get_total_memory();

        char mem_str[64] = "RAM Free: ";
        uint_to_str(free_mem / 1024 / 1024, num_buf);
        wm_str_cat(mem_str, num_buf);
        wm_str_cat(mem_str, "MB / ");
        uint_to_str(total / 1024 / 1024, num_buf);
        wm_str_cat(mem_str, num_buf);
        wm_str_cat(mem_str, "MB");
        memory_widget->setText(mem_str);

        /* Draw background glass surface */
        comp_draw_glass_surface(x, y, w, h, 12, THEME_WINDOW_BG, 15, 0x30FFFFFF);

        /* Render layout elements */
        QWidget::paint();
    }
};

/* Global shell C++ instances */
static QStatusPanel* s_panel = nullptr;
static QDock* s_dock = nullptr;
static QControlCenter* s_ctrl = nullptr;
static QNotificationCenter* s_notif = nullptr;

extern "C" {

void wm_init_shell_widgets(void) {
    uint32_t sw = comp_get_width();
    uint32_t sh = comp_get_height();

    /* Status Panel */
    s_panel = new QStatusPanel(0, 0, sw, THEME_PANEL_HEIGHT);
    s_panel->show();

    /* Dock panel */
    uint32_t dock_w = 220;
    uint32_t dock_h = THEME_DOCK_HEIGHT;
    int32_t dock_x = (sw - dock_w) / 2;
    int32_t dock_y = sh - dock_h - 15;
    s_dock = new QDock(dock_x, dock_y, dock_w, dock_h);
    s_dock->show();

    /* Control center */
    s_ctrl = new QControlCenter(sw - 290, THEME_PANEL_HEIGHT + 5, 280, 380);
    s_ctrl->show();

    /* Notification center */
    s_notif = new QNotificationCenter(sw - 290, THEME_PANEL_HEIGHT + 5, 280, sh - THEME_PANEL_HEIGHT - 15);
    s_notif->show();
}

void wm_paint_shell_panel(void) {
    if (s_panel) {
        s_panel->paint();
    }
}

void wm_paint_shell_dock(void) {
    if (s_dock) {
        s_dock->paint();
    }
}

void wm_paint_shell_control_center(void) {
    if (s_ctrl) {
        s_ctrl->paint();
    }
}

void wm_paint_shell_notification_center(void) {
    if (s_notif) {
        s_notif->paint();
    }
}

bool wm_is_control_center_active(void) {
    if (!s_ctrl) return false;
    return g_control_panel_visible || (s_ctrl->y > (int32_t)THEME_PANEL_HEIGHT - 380);
}

bool wm_is_notification_center_active(void) {
    if (!s_notif) return false;
    return g_notification_panel_visible || (s_notif->x < (int32_t)comp_get_width());
}

void wm_handle_shell_mouse(int32_t mx, int32_t my, uint8_t buttons) {
    /* Forward click/interaction to correct shell element */
    if (s_ctrl && g_control_panel_visible &&
        mx >= s_ctrl->x && mx < s_ctrl->x + (int32_t)s_ctrl->w &&
        my >= s_ctrl->y && my < s_ctrl->y + (int32_t)s_ctrl->h) {
        s_ctrl->handle_mouse(mx, my, buttons);
        return;
    }

    if (s_notif && g_notification_panel_visible &&
        mx >= s_notif->x && mx < s_notif->x + (int32_t)s_notif->w &&
        my >= s_notif->y && my < s_notif->y + (int32_t)s_notif->h) {
        s_notif->handle_mouse(mx, my, buttons);
        return;
    }

    if (s_panel && my < (int32_t)THEME_PANEL_HEIGHT) {
        s_panel->handle_mouse(mx, my, buttons);
        return;
    }

    if (s_dock &&
        mx >= s_dock->x && mx < s_dock->x + (int32_t)s_dock->w &&
        my >= s_dock->y && my < s_dock->y + (int32_t)s_dock->h) {
        s_dock->handle_mouse(mx, my, buttons);
        return;
    }
}

double sqrt_double(double val) {
    if (val <= 0) return 0;
    double x = val;
    double last;
    do {
        last = x;
        x = (x + val / x) * 0.5;
    } while (x != last);
    return x;
}
}
