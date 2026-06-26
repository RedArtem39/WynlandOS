/*
 * WynlandOS - Window Manager and GUI main loop
 * ============================================================
 */
#include "wm.h"
#include "compositor.h"
#include "theme.h"
#include <wynland/heap.h>
#include <wynland/irq.h>
#include <wynland/mouse.h>

extern void serial_write_string(const char *str);
extern void uint_to_str(uint64_t val, char *buf);
extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);

/* Globally accessible console dimensions from main.c */
extern uint32_t console_start_x;
extern uint32_t console_start_y;
extern uint32_t console_end_x;
extern uint32_t console_end_y;
extern uint32_t cursor_x;
extern uint32_t cursor_y;
extern uint32_t term_bg_color;

/* Stacking window list */
static Window *g_windows_head = NULL; /* Top-most / Focused window */
static Window *g_windows_tail = NULL; /* Bottom-most window */

static Window *g_dragged_window = NULL;
static int32_t g_drag_offset_x = 0;
static int32_t g_drag_offset_y = 0;

static Window *g_resizing_window = NULL;
static uint32_t g_resize_start_w = 0;
static uint32_t g_resize_start_h = 0;
static int32_t g_resize_start_mx = 0;
static int32_t g_resize_start_my = 0;

static bool g_gui_active = false;
static bool g_wm_needs_redraw = true;
static uint8_t g_prev_buttons = 0;
static Window *g_term_window = NULL;
static Window *g_settings_window = NULL;

/* Terminal buffer */
#define TERM_ROWS 30
#define TERM_COLS 100
static char g_term_grid[TERM_ROWS][TERM_COLS];
static uint32_t g_term_fg_grid[TERM_ROWS][TERM_COLS];
static uint32_t g_term_bg_grid[TERM_ROWS][TERM_COLS];

/* Dock configurations */
#define DOCK_ICON_COUNT 3
typedef struct {
    char name[16];
    uint32_t color;
    int32_t cx, cy;
    uint32_t base_radius;
    uint32_t current_radius;
} DockIcon;

static DockIcon g_dock_icons[DOCK_ICON_COUNT] = {
    { "Terminal", 0x88C0D0, 0, 0, 16, 16 },
    { "Settings", 0xB48EAD, 0, 0, 16, 16 },
    { "Browser",  0xD08770, 0, 0, 16, 16 }
};

/* Forward declarations */
static void draw_terminal_content(Window *self);
static void draw_settings_content(Window *self);

bool wm_is_gui_active(void)
{
    return g_gui_active;
}

void wm_exit_gui(void)
{
    g_gui_active = false;
}

void wm_mark_dirty(void)
{
    g_wm_needs_redraw = true;
}

void wm_term_set_cell(int row, int col, char c, uint32_t fg, uint32_t bg)
{
    if (row >= 0 && row < TERM_ROWS && col >= 0 && col < TERM_COLS) {
        g_term_grid[row][col] = c;
        g_term_fg_grid[row][col] = fg;
        g_term_bg_grid[row][col] = bg;
    }
}

void wm_term_scroll(void)
{
    for (int r = 0; r < TERM_ROWS - 1; r++) {
        memcpy(g_term_grid[r], g_term_grid[r + 1], TERM_COLS);
        memcpy(g_term_fg_grid[r], g_term_fg_grid[r + 1], TERM_COLS * 4);
        memcpy(g_term_bg_grid[r], g_term_bg_grid[r + 1], TERM_COLS * 4);
    }
    memset(g_term_grid[TERM_ROWS - 1], 0, TERM_COLS);
    comp_mark_dirty();
}

void wm_term_clear_line(int row)
{
    if (row >= 0 && row < TERM_ROWS) {
        memset(g_term_grid[row], 0, TERM_COLS);
        if (g_term_window) {
            int32_t cx = g_term_window->x + 1;
            int32_t cy = g_term_window->y + THEME_TITLEBAR_HEIGHT + 1;
            int32_t line_y = cy + 10 + row * 18;
            int32_t line_x = cx + 10;
            uint32_t line_w = g_term_window->w - 20;

            extern void comp_draw_wallpaper_rect(uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh);
            comp_draw_wallpaper_rect(line_x, line_y, line_w, 16);
            comp_fill_rect_alpha(line_x, line_y, line_w, 16, THEME_TERM_BG);
        }
    }
}

void wm_term_clear(void)
{
    memset(g_term_grid, 0, sizeof(g_term_grid));
    comp_mark_dirty();
}

/* Add window to the top of Z-order */
void wm_register_window(Window *win)
{
    win->next = g_windows_head;
    win->prev = NULL;
    if (g_windows_head) {
        g_windows_head->prev = win;
    }
    g_windows_head = win;
    if (!g_windows_tail) {
        g_windows_tail = win;
    }
    comp_mark_dirty();
}

/* Raise window to the top (focused) */
void wm_raise_window(Window *win)
{
    if (win == g_windows_head) return;

    /* Unlink */
    if (win->prev) win->prev->next = win->next;
    if (win->next) win->next->prev = win->prev;
    if (win == g_windows_tail) g_windows_tail = win->prev;

    /* Link to head */
    win->next = g_windows_head;
    win->prev = NULL;
    if (g_windows_head) g_windows_head->prev = win;
    g_windows_head = win;

    /* Update focus flags */
    Window *curr = g_windows_head;
    while (curr) {
        curr->is_focused = (curr == win);
        curr = curr->next;
    }

    /* Synchronize terminal console print area with this window if it is the Terminal */
    if (win->id == 1) {
        console_start_x = win->x + 15;
        console_start_y = win->y + 40;
        console_end_x = win->x + win->w - 15;
        console_end_y = win->y + win->h - 15;
        term_bg_color = THEME_WINDOW_BG;
    }

    comp_mark_dirty();
}

void wm_init(void)
{
    serial_write_string("WM: Initializing Stacking Window Manager...\r\n");

    g_windows_head = NULL;
    g_windows_tail = NULL;
    g_gui_active = true;

    /* Create Terminal Window (ID 1) */
    Window *term = (Window *)kmalloc(sizeof(Window));
    term->id = 1;
    term->x = 60;
    term->y = 80;
    term->w = 580;
    term->h = 420;
    memcpy(term->title, "WynlandOS Terminal", 19);
    term->is_visible = true;
    term->is_focused = true;
    term->draw_content = draw_terminal_content;
    g_term_window = term;
    wm_register_window(term);

    /* Create Settings Window (ID 2) */
    Window *settings = (Window *)kmalloc(sizeof(Window));
    settings->id = 2;
    settings->x = 420;
    settings->y = 140;
    settings->w = 340;
    settings->h = 260;
    memcpy(settings->title, "System Settings", 16);
    settings->is_visible = true;
    settings->is_focused = false;
    settings->draw_content = draw_settings_content;
    g_settings_window = settings;
    wm_register_window(settings);

    /* Set up terminal buffer character step mapping */
    console_start_x = term->x + 15;
    console_start_y = term->y + 40;
    console_end_x = term->x + term->w - 15;
    console_end_y = term->y + term->h - 15;
    term_bg_color = THEME_WINDOW_BG;

    /* Raise Terminal to make it default focused */
    wm_raise_window(term);

    serial_write_string("WM: Initialized successfully.\r\n");
}

/* ============================================================
 * Window Content Drawing Callbacks
 * ============================================================ */

static void draw_terminal_content(Window *self)
{
    /* Client area */
    int32_t cx = self->x + 1;
    int32_t cy = self->y + THEME_TITLEBAR_HEIGHT + 1;
    uint32_t cw = self->w - 2;
    uint32_t ch = self->h - THEME_TITLEBAR_HEIGHT - 2;

    /* Fill background with alpha */
    comp_fill_rect_alpha(cx, cy, cw, ch, THEME_TERM_BG);

    /* Render characters inside viewport */
    for (int r = 0; r < TERM_ROWS; r++) {
        int32_t char_y = cy + 10 + r * 18;
        if (char_y + 16 > (int32_t)(self->y + self->h - 10)) break;

        for (int c = 0; c < TERM_COLS; c++) {
            int32_t char_x = cx + 10 + c * 9;
            if (char_x + 8 > (int32_t)(self->x + self->w - 10)) break;

            char ch = g_term_grid[r][c];
            if (ch != '\0') {
                uint32_t fg = g_term_fg_grid[r][c];
                uint32_t bg = g_term_bg_grid[r][c];
                comp_draw_char(char_x, char_y, ch, fg, bg);
            }
        }
    }
}

static void draw_settings_content(Window *self)
{
    /* Client area */
    int32_t cx = self->x + 1;
    int32_t cy = self->y + THEME_TITLEBAR_HEIGHT + 1;
    uint32_t cw = self->w - 2;
    uint32_t ch = self->h - THEME_TITLEBAR_HEIGHT - 2;

    /* Fill background with alpha */
    comp_fill_rect_alpha(cx, cy, cw, ch, THEME_WINDOW_BG);

    /* Title of settings panel */
    comp_draw_string(cx + 20, cy + 20, "WynlandOS Configuration", THEME_TEXT_PRIMARY & 0x00FFFFFF, 0);

    /* Draw some pretty mock settings items (Nord color palette demonstration) */
    comp_draw_string(cx + 20, cy + 50, "Desktop Theme:", THEME_TEXT_SECONDARY & 0x00FFFFFF, 0);
    comp_draw_rounded_rect(cx + 140, cy + 47, 80, 20, 4, NORD8 & 0x00FFFFFF);
    comp_draw_string(cx + 155, cy + 49, "Nord", THEME_WINDOW_BG & 0x00FFFFFF, 0);

    comp_draw_string(cx + 20, cy + 80, "Window Mode:", THEME_TEXT_SECONDARY & 0x00FFFFFF, 0);
    comp_draw_rounded_rect(cx + 140, cy + 77, 80, 20, 4, NORD3 & 0x00FFFFFF);
    comp_draw_string(cx + 150, cy + 79, "Stacking", THEME_TEXT_PRIMARY & 0x00FFFFFF, 0);

    /* Draw colored boxes to represent Nord aurora color palette */
    comp_draw_string(cx + 20, cy + 120, "Aurora Colors:", THEME_TEXT_SECONDARY & 0x00FFFFFF, 0);
    comp_fill_rect(cx + 20, cy + 140, 24, 24, NORD11 & 0x00FFFFFF);
    comp_fill_rect(cx + 50, cy + 140, 24, 24, NORD12 & 0x00FFFFFF);
    comp_fill_rect(cx + 80, cy + 140, 24, 24, NORD13 & 0x00FFFFFF);
    comp_fill_rect(cx + 110, cy + 140, 24, 24, NORD14 & 0x00FFFFFF);
    comp_fill_rect(cx + 140, cy + 140, 24, 24, NORD15 & 0x00FFFFFF);
}

/* ============================================================
 * Desktop Render and Events
 * ============================================================ */

static inline uint64_t save_irq_disable(void)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));
    return rflags;
}

static inline void restore_irq(uint64_t rflags)
{
    if (rflags & 0x200) {
        __asm__ volatile("sti");
    }
}

void wm_draw_desktop(void)
{
    static uint64_t last_sec = 999999;
    uint64_t total_sec = timer_get_ticks() / 100;

    /* Force redraw every second to update clock */
    if (total_sec != last_sec) {
        g_wm_needs_redraw = true;
        last_sec = total_sec;
    }

    int32_t mx = mouse_get_x();
    int32_t my = mouse_get_y();
    uint8_t buttons = mouse_get_buttons();

    static int32_t last_mx = -1;
    static int32_t last_my = -1;
    static uint8_t last_buttons = 0;

    /* Lock interrupts to make desktop drawing and mouse updates atomic and prevent races */
    uint64_t rflags = save_irq_disable();

    if (!g_wm_needs_redraw) {
        /* If only the mouse moved, buttons changed, or compositor is dirty, do lightweight flip */
        extern bool comp_is_dirty(void);
        if (mx != last_mx || my != last_my || buttons != last_buttons || comp_is_dirty()) {
            /* 1. Restore old background under cursor */
            comp_restore_cursor_back();

            /* 2. Draw cursor at new position */
            comp_draw_cursor(mx, my, buttons);

            /* 3. Flip only the dirty regions (old/new cursor + other dirty regions) */
            compositor_flip();

            last_mx = mx;
            last_my = my;
            last_buttons = buttons;
        }
        restore_irq(rflags);
        return;
    }

    /* Full desktop redraw */
    g_wm_needs_redraw = false;

    uint32_t sw = comp_get_width();
    uint32_t sh = comp_get_height();

    /* 1. Wallpaper (clears any saved cursor state since we reconstruct from scratch) */
    comp_clear_saved_cursor();
    comp_draw_wallpaper();

    /* 2. Draw all windows in reverse Z-order (bottom-most to top-most) */
    Window *win = g_windows_tail;
    while (win) {
        if (win->is_visible) {
            draw_window_decorations(win);
            if (win->draw_content) {
                win->draw_content(win);
            }
            /* Draw a resize handle in bottom-right corner of window */
            if (win->is_focused) {
                int32_t rx = win->x + win->w - 12;
                int32_t ry = win->y + win->h - 12;
                comp_fill_rect(rx, ry, 8, 8, THEME_ACCENT & 0x00FFFFFF);
            }
        }
        win = win->prev;
    }

    /* Check if dragging or resizing is active to bypass blur computations for 60fps */
    bool dragging = (g_dragged_window != NULL || g_resizing_window != NULL);

    /* 3. Top panel */
    if (!dragging) {
        extern void comp_box_blur(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t radius);
        comp_box_blur(0, 0, sw, THEME_PANEL_HEIGHT, 1);
    }
    comp_draw_panel();

    /* Draw actual clocks on top panel */
    uint32_t sec = total_sec % 60;
    uint32_t min = (total_sec / 60) % 60;
    uint32_t hr  = (total_sec / 3600) % 24;

    char time_str[9];
    time_str[0] = '0' + (hr / 10);
    time_str[1] = '0' + (hr % 10);
    time_str[2] = ':';
    time_str[3] = '0' + (min / 10);
    time_str[4] = '0' + (min % 10);
    time_str[5] = ':';
    time_str[6] = '0' + (sec / 10);
    time_str[7] = '0' + (sec % 10);
    time_str[8] = '\0';
    comp_draw_string(sw - 80, 4, time_str, THEME_TEXT_PRIMARY & 0x00FFFFFF, 0);

    /* 4. Draw Dock Panel with Magnification */
    uint32_t dock_w = 220;
    uint32_t dock_h = THEME_DOCK_HEIGHT;
    int32_t dock_x = (sw - dock_w) / 2;
    int32_t dock_y = sh - dock_h - 15;

    /* Blur under Dock if not dragging */
    if (!dragging) {
        comp_box_blur(dock_x, dock_y, dock_w, dock_h, 2);
    }

    /* Draw semi-transparent dock background */
    comp_fill_rect_alpha(dock_x, dock_y, dock_w, dock_h, THEME_DOCK_BG);
    comp_draw_rounded_rect(dock_x, dock_y, dock_w, dock_h, 12, THEME_DOCK_BG);

    /* Draw dock icons with magnification effect */
    int spacing = dock_w / (DOCK_ICON_COUNT + 1);
    for (int i = 0; i < DOCK_ICON_COUNT; i++) {
        g_dock_icons[i].cx = dock_x + spacing * (i + 1);
        g_dock_icons[i].cy = dock_y + dock_h / 2;

        /* Calculate distance from mouse to icon center */
        int32_t dx = mx - g_dock_icons[i].cx;
        int32_t dy = my - g_dock_icons[i].cy;
        int32_t dist_sq = dx * dx + dy * dy;

        /* Magnification logic: icons get larger if mouse is near */
        if (dist_sq < 80 * 80) {
            /* Linear scale factor based on proximity */
            uint32_t zoom = (80 * 80 - dist_sq) / 400; /* up to +16px */
            g_dock_icons[i].current_radius = g_dock_icons[i].base_radius + zoom;
        } else {
            g_dock_icons[i].current_radius = g_dock_icons[i].base_radius;
        }

        /* Draw icon circle */
        comp_draw_circle(g_dock_icons[i].cx, g_dock_icons[i].cy, g_dock_icons[i].current_radius, g_dock_icons[i].color);
        
        /* Draw little dot under active app */
        if (i == 0) { /* Terminal is running */
            comp_draw_circle(g_dock_icons[i].cx, dock_y + dock_h - 4, 2, THEME_ACCENT & 0x00FFFFFF);
        }
    }

    /* 5. Draw cursor on the back-buffer right before flipping */
    comp_draw_cursor(mx, my, buttons);

    /* Flip onto screen */
    compositor_flip();

    last_mx = mx;
    last_my = my;
    last_buttons = buttons;

    restore_irq(rflags);
}

void wm_handle_mouse(int32_t mx, int32_t my, uint8_t buttons)
{
    static int32_t g_prev_my = 0;
    bool left_pressed  = (buttons & 1) != 0;
    bool prev_left     = (g_prev_buttons & 1) != 0;
    bool clicked_down  = left_pressed && !prev_left;
    bool clicked_up    = !left_pressed && prev_left;

    bool dragging = (g_dragged_window != NULL || g_resizing_window != NULL);

    if (clicked_down) {
        /* Check if clicked on a window */
        Window *win = g_windows_head;
        bool handled = false;

        while (win) {
            if (win->is_visible) {
                /* Check boundary */
                if (mx >= win->x && mx < (int32_t)(win->x + win->w) &&
                    my >= win->y && my < (int32_t)(win->y + win->h)) {
                    
                    /* Click is inside the window! Raise it to front */
                    wm_raise_window(win);
                    handled = true;

                    /* 1. Close button hit */
                    int32_t close_dx = mx - (win->x + 18);
                    int32_t close_dy = my - (win->y + 14);
                    if (close_dx * close_dx + close_dy * close_dy <= 6 * 6) {
                        if (win->id == 1) {
                            /* Exit GUI mode if terminal is closed */
                            wm_exit_gui();
                        } else {
                            win->is_visible = false;
                            comp_mark_dirty();
                        }
                        break;
                    }

                    /* 2. Dragging titlebar */
                    if (my < (int32_t)(win->y + THEME_TITLEBAR_HEIGHT)) {
                        g_dragged_window = win;
                        g_drag_offset_x = mx - win->x;
                        g_drag_offset_y = my - win->y;
                    }

                    /* 3. Resize handle in bottom-right corner */
                    int32_t rx = win->x + win->w - 12;
                    int32_t ry = win->y + win->h - 12;
                    if (mx >= rx && mx < (int32_t)(win->x + win->w) &&
                        my >= ry && my < (int32_t)(win->y + win->h)) {
                        g_resizing_window = win;
                        g_resize_start_w = win->w;
                        g_resize_start_h = win->h;
                        g_resize_start_mx = mx;
                        g_resize_start_my = my;
                    }

                    break; /* Found clicked window, skip others underneath */
                }
            }
            win = win->next;
        }

        /* If clicked outside windows, check Dock Icons click */
        if (!handled) {
            uint32_t sh = comp_get_height();
            uint32_t dock_h = THEME_DOCK_HEIGHT;
            int32_t dock_y = sh - dock_h - 15;
            if (my >= dock_y && my < (int32_t)(dock_y + dock_h)) {
                for (int i = 0; i < DOCK_ICON_COUNT; i++) {
                    int32_t dx = mx - g_dock_icons[i].cx;
                    int32_t dy = my - g_dock_icons[i].cy;
                    if (dx*dx + dy*dy <= (int32_t)(g_dock_icons[i].current_radius * g_dock_icons[i].current_radius)) {
                        /* Action! Launch/Focus window */
                        if (i == 0 && g_term_window) {
                            g_term_window->is_visible = true;
                            wm_raise_window(g_term_window);
                        } else if (i == 1 && g_settings_window) {
                            g_settings_window->is_visible = true;
                            wm_raise_window(g_settings_window);
                        }
                        break;
                    }
                }
            }
        }
    }

    /* Dragging action */
    if (left_pressed && g_dragged_window) {
        g_dragged_window->x = mx - g_drag_offset_x;
        g_dragged_window->y = my - g_drag_offset_y;

        /* Clamp y to panel height */
        if (g_dragged_window->y < (int32_t)THEME_PANEL_HEIGHT) {
            g_dragged_window->y = THEME_PANEL_HEIGHT;
        }

        /* Dynamically synchronize shell text boundaries with moving window */
        if (g_dragged_window->id == 1) {
            console_start_x = g_dragged_window->x + 15;
            console_start_y = g_dragged_window->y + 40;
            console_end_x = g_dragged_window->x + g_dragged_window->w - 15;
            console_end_y = g_dragged_window->y + g_dragged_window->h - 15;
        }

        comp_mark_dirty();
    }

    /* Resizing action */
    if (left_pressed && g_resizing_window) {
        int32_t dw = mx - g_resize_start_mx;
        int32_t dh = my - g_resize_start_my;

        int32_t new_w = (int32_t)g_resize_start_w + dw;
        int32_t new_h = (int32_t)g_resize_start_h + dh;

        if (new_w < 180) new_w = 180;
        if (new_h < 120) new_h = 120;

        g_resizing_window->w = new_w;
        g_resizing_window->h = new_h;

        /* Dynamically synchronize console text boundaries with resized window */
        if (g_resizing_window->id == 1) {
            console_start_x = g_resizing_window->x + 15;
            console_start_y = g_resizing_window->y + 40;
            console_end_x = g_resizing_window->x + g_resizing_window->w - 15;
            console_end_y = g_resizing_window->y + g_resizing_window->h - 15;
        }

        comp_mark_dirty();
    }

    /* Release drag/resize locks */
    if (clicked_up) {
        g_dragged_window = NULL;
        g_resizing_window = NULL;
    }

    g_prev_buttons = buttons;

    /* Detect if we are close to the dock to trigger animation updates */
    bool near_dock = (my >= 650) || (g_prev_my >= 650);
    g_prev_my = my;

    /* Only trigger a full desktop redraw if something major changed (clicks, drag/resize active, near dock) */
    if (clicked_down || clicked_up || (left_pressed && dragging) || near_dock) {
        comp_mark_dirty();
    }
}

void wm_handle_key(uint8_t scancode, char ascii)
{
    /* Deliver key presses only to focused window */
    if (g_windows_head && g_windows_head->is_focused && g_windows_head->is_visible) {
        if (g_windows_head->handle_key) {
            g_windows_head->handle_key(g_windows_head, scancode, ascii);
        }
    }
}
