/*
 * WynlandOS - Window Manager and GUI main loop
 * ============================================================
 */
#include "wm.h"

extern "C" {
#include "compositor.h"
#include "theme.h"
#include <wynland/heap.h>
#include <wynland/irq.h>
#include <wynland/mouse.h>
#include <wynland/net.h>
#include <wynland/http.h>

void serial_write_string(const char *str);
void uint_to_str(uint64_t val, char *buf);
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);

void beep(uint32_t freq, uint32_t duration_ms);
void nosound(void);
void play_startup_chime(void);
void sys_reboot(void);
void sys_poweroff(void);

void wm_paint_cpp_widgets(void* root);
void wm_set_widgets_root_pos(void* root, int32_t x, int32_t y);
void wm_init_settings_widgets(void* settings_win_ptr);
}

/* Globally accessible console dimensions from main.c */
extern uint32_t console_start_x;
extern uint32_t console_start_y;
extern uint32_t console_end_x;
extern uint32_t console_end_y;
extern uint32_t cursor_x;
extern uint32_t cursor_y;
extern uint32_t term_bg_color;

void draw_forge_content(Window *self);
void handle_forge_key(Window *self, uint8_t scancode, char ascii);
void handle_forge_mouse(Window *self, int32_t mx, int32_t my, uint8_t buttons);

/* Stacking window list */
static Window *g_windows_head = NULL; /* Top-most / Focused window */
static Window *g_windows_tail = NULL; /* Bottom-most window */

Window *g_dragged_window = NULL;
static int32_t g_drag_offset_x = 0;
static int32_t g_drag_offset_y = 0;

Window *g_resizing_window = NULL;
static uint32_t g_resize_start_w = 0;
static uint32_t g_resize_start_h = 0;
static int32_t g_resize_start_mx = 0;
static int32_t g_resize_start_my = 0;
Window *g_mouse_captured_window = NULL;

/* outline variables removed for real-time dragging */

static bool g_gui_active = false;
static bool g_wm_needs_redraw = true;
static uint8_t g_prev_buttons = 0;
static Window *g_term_window = NULL;
static Window *g_settings_window = NULL;
static Window *g_browser_window = NULL;
static Window *g_forge_window = NULL;
/*
static char g_browser_url[256] = "10.0.2.2:8000";
static char g_browser_content_buf[4096] = {0};
*/

/* Terminal buffer */
#define TERM_ROWS 300
#define TERM_COLS 100
static uint16_t g_term_grid[TERM_ROWS][TERM_COLS];
static uint32_t g_term_fg_grid[TERM_ROWS][TERM_COLS];
static uint32_t g_term_bg_grid[TERM_ROWS][TERM_COLS];
static int32_t g_term_view_offset = 0;
static int32_t g_term_user_scroll = 0;

/* Dock configurations */
#define DOCK_ICON_COUNT 4
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
    { "Browser",  0xD08770, 0, 0, 16, 16 },
    { "Forge",    0xA3BE8C, 0, 0, 16, 16 }
};

/* Forward declarations */
static void draw_terminal_content(Window *self);
static void draw_settings_content(Window *self);
/*
static void draw_browser_content(Window *self);
static void handle_browser_key(Window *self, uint8_t scancode, char ascii);
static void handle_browser_mouse(Window *self, int32_t mx, int32_t my, uint8_t buttons);
static void browser_load_page(void);
*/

extern void comp_draw_icon_forge(int32_t cx, int32_t cy, int32_t r);

static void draw_dock_icon(int i, int32_t cx, int32_t cy, int32_t r)
{
    if (i == 0) {
        comp_draw_icon_terminal(cx, cy, r);
    } else if (i == 1) {
        comp_draw_icon_settings(cx, cy, r);
    } else if (i == 2) {
        comp_draw_icon_browser(cx, cy, r);
    } else if (i == 3) {
        comp_draw_icon_forge(cx, cy, r);
    } else {
        comp_draw_circle(cx, cy, r, g_dock_icons[i].color);
    }
}

extern "C" {
bool wm_is_gui_active(void)
{
    return g_gui_active;
}

void wm_exit_gui(void)
{
    g_gui_active = false;
}

bool wm_is_terminal_focused(void)
{
    return (g_windows_head == g_term_window);
}

void wm_mark_dirty(void)
{
    g_wm_needs_redraw = true;
}

void wm_term_set_cell(int row, int col, uint16_t c, uint32_t fg, uint32_t bg)
{
    int abs_row = g_term_view_offset + row;
    if (abs_row >= 0 && abs_row < TERM_ROWS && col >= 0 && col < TERM_COLS) {
        g_term_grid[abs_row][col] = c;
        g_term_fg_grid[abs_row][col] = fg;
        g_term_bg_grid[abs_row][col] = bg;
        
        /* Auto scroll to bottom when new characters are printed */
        g_term_user_scroll = g_term_view_offset;
    }
}

void wm_term_scroll(void)
{
    extern uint32_t cursor_y;
    
    if (cursor_y < TERM_ROWS) {
        g_term_view_offset++;
    } else {
        /* Shift the entire history buffer up by 1 row */
        for (int r = 0; r < TERM_ROWS - 1; r++) {
            memcpy(g_term_grid[r], g_term_grid[r + 1], sizeof(g_term_grid[r]));
            memcpy(g_term_fg_grid[r], g_term_fg_grid[r + 1], TERM_COLS * 4);
            memcpy(g_term_bg_grid[r], g_term_bg_grid[r + 1], TERM_COLS * 4);
        }
        cursor_y--;
    }
    
    int new_bottom_row = g_term_view_offset + 19; /* viewport size is 20 rows */
    if (new_bottom_row < TERM_ROWS) {
        memset(g_term_grid[new_bottom_row], 0, sizeof(g_term_grid[0]));
    }
    
    g_term_user_scroll = g_term_view_offset;
    comp_mark_dirty();
}

void wm_term_clear_line(int row)
{
    int abs_row = g_term_view_offset + row;
    if (abs_row >= 0 && abs_row < TERM_ROWS) {
        memset(g_term_grid[abs_row], 0, sizeof(g_term_grid[abs_row]));
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
    extern uint32_t cursor_y;
    memset(g_term_grid, 0, sizeof(g_term_grid));
    g_term_view_offset = 0;
    g_term_user_scroll = 0;
    cursor_y = 0;
    comp_mark_dirty();
}
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
    win->prev_x = win->x;
    win->prev_y = win->y;
    win->prev_w = win->w;
    win->prev_h = win->h;

    /* Initialize spring physics states */
    if (win->is_visible) {
        win->scale_spring.current = 256;
        win->scale_spring.target = 256;
        win->scale_spring.velocity = 0;
    } else {
        win->scale_spring.current = 0;
        win->scale_spring.target = 0;
        win->scale_spring.velocity = 0;
    }
    win->anim_direction = 0;

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
        term_bg_color = 0;
    }

    comp_mark_dirty();
}

extern "C" {
int32_t wm_get_browser_x(void) { return g_browser_window ? g_browser_window->x : 0; }
int32_t wm_get_browser_y(void) { return g_browser_window ? g_browser_window->y : 0; }
int32_t wm_get_browser_w(void) { return g_browser_window ? g_browser_window->w : 0; }
int32_t wm_get_browser_h(void) { return g_browser_window ? g_browser_window->h : 0; }

void draw_browser_content_wlang(Window *self) {
    (void)self;
    extern void wlang_call_on_draw(void);
    wlang_call_on_draw();
}

void handle_browser_key_wlang(Window *self, uint8_t scancode, char ascii) {
    (void)self;
    extern void wlang_call_on_key(uint8_t scancode, char ascii);
    wlang_call_on_key(scancode, ascii);
}

void handle_browser_mouse_wlang(Window *self, int32_t mx, int32_t my, uint8_t buttons) {
    (void)self;
    extern void wlang_call_on_mouse(int32_t mx, int32_t my, uint8_t buttons);
    wlang_call_on_mouse(mx, my, buttons);
}
}

static void handle_term_key(Window *self, uint8_t scancode, char ascii)
{
    (void)self; (void)ascii;
    if (scancode == 0x49) { /* Page Up */
        if (g_term_user_scroll > 0) {
            g_term_user_scroll -= 5;
            if (g_term_user_scroll < 0) g_term_user_scroll = 0;
            comp_mark_dirty();
        }
    }
    else if (scancode == 0x51) { /* Page Down */
        if (g_term_user_scroll < g_term_view_offset) {
            g_term_user_scroll += 5;
            if (g_term_user_scroll > g_term_view_offset) g_term_user_scroll = g_term_view_offset;
            comp_mark_dirty();
        }
    }
}

extern "C" void wm_init(void)
{
    serial_write_string("WM: Initializing Stacking Window Manager...\r\n");

    g_windows_head = NULL;
    g_windows_tail = NULL;
    g_gui_active = true;

    /* Create Terminal Window (ID 1) */
    Window *term = new Window(1, 60, 80, 580, 420, "WynlandOS Terminal");
    term->is_visible = true;
    term->is_focused = true;
    term->draw_content = draw_terminal_content;
    term->handle_key_cb = handle_term_key;
    g_term_window = term;
    wm_register_window(term);

    /* Create Settings Window (ID 2) */
    Window *settings = new Window(2, 420, 140, 340, 260, "System Settings");
    settings->is_visible = true;
    settings->is_focused = false;
    settings->draw_content = draw_settings_content;
    
    /* Initialize C++ Widgets container for Settings window */
    wm_init_settings_widgets(settings);

    g_settings_window = settings;
    wm_register_window(settings);

    /* Create Browser Window (ID 3) */
    Window *browser = new Window(3, 100, 120, 540, 360, "WynlandOS Browser");
    browser->is_visible = false; /* Starts hidden, open from Dock */
    browser->is_focused = false;
    browser->draw_content = draw_browser_content_wlang;
    browser->handle_key_cb = handle_browser_key_wlang;
    browser->handle_mouse_cb = handle_browser_mouse_wlang;
    g_browser_window = browser;
    wm_register_window(browser);

    /* Create Forge Window (ID 4) */
    Window *forge = new Window(4, 180, 100, 500, 340, "Forge File Explorer");
    forge->is_visible = false; /* Starts hidden, open from Dock */
    forge->is_focused = false;

    forge->draw_content = draw_forge_content;
    forge->handle_key_cb = handle_forge_key;
    forge->handle_mouse_cb = handle_forge_mouse;
    g_forge_window = forge;
    wm_register_window(forge);

    /* Set up terminal buffer character step mapping */
    console_start_x = term->x + 15;
    console_start_y = term->y + 40;
    console_end_x = term->x + term->w - 15;
    console_end_y = term->y + term->h - 15;
    term_bg_color = 0;

    /* Raise Terminal to make it default focused */
    wm_raise_window(term);

    extern void wlang_browser_init(void);
    wlang_browser_init();

    serial_write_string("WM: Initialized successfully.\r\n");

    /* Play the startup chime sound chord */
    extern void play_startup_chime(void);
    play_startup_chime();
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

    /* Fill background with alpha, or solid if maximized */
    if (self->is_maximized) {
        comp_fill_rect(cx, cy, cw, ch, THEME_TERM_BG & 0x00FFFFFF);
    } else {
        comp_fill_rect_alpha(cx, cy, cw, ch, THEME_TERM_BG);
    }

    /* Render characters inside viewport */
    int32_t visible_r = 0;
    for (int r = g_term_user_scroll; r < TERM_ROWS; r++) {
        int32_t char_y = cy + 10 + visible_r * 18;
        if (char_y + 16 > (int32_t)(self->y + self->h - 10)) break;

        for (int c = 0; c < TERM_COLS; c++) {
            int32_t char_x = cx + 10 + c * 9;
            if (char_x + 8 > (int32_t)(self->x + self->w - 10)) break;

            uint16_t ch = g_term_grid[r][c];
            if (ch != '\0') {
                uint32_t fg = g_term_fg_grid[r][c];
                uint32_t bg = g_term_bg_grid[r][c];
                comp_draw_char(char_x, char_y, ch, fg, bg);
            }
        }
        visible_r++;
    }
}

static void draw_settings_content(Window *self)
{
    /* Client area */
    int32_t cx = self->x + 1;
    int32_t cy = self->y + THEME_TITLEBAR_HEIGHT + 1;
    uint32_t cw = self->w - 2;
    uint32_t ch = self->h - THEME_TITLEBAR_HEIGHT - 2;

    /* Fill background with alpha, or solid if maximized */
    if (self->is_maximized) {
        comp_fill_rect(cx, cy, cw, ch, THEME_WINDOW_BG & 0x00FFFFFF);
    } else {
        comp_fill_rect_alpha(cx, cy, cw, ch, THEME_WINDOW_BG);
    }

    if (self->cpp_widgets_root) {
        wm_set_widgets_root_pos(self->cpp_widgets_root, cx, cy);
        wm_paint_cpp_widgets(self->cpp_widgets_root);
    }
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

static void wm_str_cat(char *dest, const char *src)
{
    while (*dest) dest++;
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

static void wm_draw_panel_contents(uint32_t sw, uint32_t total_sec)
{
    /* Restore wallpaper under the entire panel to prevent alpha accumulation */
    extern void comp_draw_wallpaper_rect(uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh);
    comp_draw_wallpaper_rect(0, 0, sw, THEME_PANEL_HEIGHT);

    /* Draw base panel background and separator border */
    comp_draw_panel();

    uint32_t fg = THEME_TEXT_PRIMARY & 0x00FFFFFF;
    uint32_t pipe_fg = NORD3 & 0x00FFFFFF;

    /* 1. Left side: WynlandOS Logo & Name */
    comp_draw_string(10, 4, "W", THEME_ACCENT & 0x00FFFFFF, 0);
    comp_draw_string(19, 4, "ynlandOS", fg, 0);

    /* 2. Active application / window title */
    extern Window *g_windows_head;
    const char *active_title = "Desktop";
    if (g_windows_head && g_windows_head->is_visible && g_windows_head->scale_spring.target == 256) {
        active_title = g_windows_head->title;
    }
    comp_draw_string(110, 4, "|", pipe_fg, 0);
    comp_draw_string(125, 4, active_title, NORD8 & 0x00FFFFFF, 0);

    /* 3. Keyboard Layout Indicator */
    extern bool layout_ru;
    comp_draw_string(sw - 130, 4, "|", pipe_fg, 0);
    comp_draw_string(sw - 110, 4, layout_ru ? "RU" : "EN", THEME_TEXT_SECONDARY & 0x00FFFFFF, 0);

    /* 4. RAM / Heap memory stats */
    extern size_t heap_get_used_memory(void);
    size_t used_kb = heap_get_used_memory() / 1024;
    char ram_str[32] = "RAM: ";
    char used_str[16];
    extern void uint_to_str(uint64_t val, char *buf);
    uint_to_str((uint64_t)used_kb, used_str);
    wm_str_cat(ram_str, used_str);
    wm_str_cat(ram_str, " KB");
    comp_draw_string(sw - 230, 4, ram_str, NORD7 & 0x00FFFFFF, 0);
    comp_draw_string(sw - 245, 4, "|", pipe_fg, 0);

    /* 5. Right side: Clock time */
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
    comp_draw_string(sw - 75, 4, time_str, fg, 0);
}

static bool g_control_panel_visible = false;

static void wm_draw_control_panel(uint32_t sw, uint32_t sh)
{
    (void)sh;
    int32_t px = sw - 290;
    int32_t py = THEME_PANEL_HEIGHT + 5;
    int32_t pw = 280;
    int32_t ph = 360;

    /* Shadow */
    comp_fill_rect_alpha(px + 4, py + 4, pw, ph, 0x30000000);

    /* Background panel */
    comp_draw_rounded_rect(px, py, pw, ph, 12, 0xE520242C); /* Nord dark with 90% alpha */
    comp_draw_rounded_rect_border(px, py, pw, ph, 12, NORD3 & 0x00FFFFFF);

    /* Header */
    comp_draw_string(px + 15, py + 15, "Control Center", NORD6 & 0x00FFFFFF, 0);
    comp_fill_rect(px + 15, py + 32, pw - 30, 1, NORD3 & 0x00FFFFFF);

    /* --- SECTION 1: CALENDAR (June 2026) --- */
    comp_draw_string(px + 15, py + 42, "June 2026", NORD8 & 0x00FFFFFF, 0);
    
    /* Weekdays header */
    comp_draw_string(px + 15, py + 62, "Mo Tu We Th Fr Sa Su", NORD4 & 0x00FFFFFF, 0);

    /* Days grid */
    const char *days[] = {
        " 1  2  3  4  5  6  7",
        " 8  9 10 11 12 13 14",
        "15 16 17 18 19 20 21",
        "22 23 24 25 26 27 28", /* 27 is Saturday */
        "29 30"
    };

    for (int i = 0; i < 5; i++) {
        comp_draw_string(px + 15, py + 82 + i * 16, days[i], NORD6 & 0x00FFFFFF, 0);
    }

    /* Highlight Saturday 27th */
    comp_fill_rect_alpha(px + 15 + 15 * 9 - 2, py + 82 + 3 * 16 - 1, 19, 14, 0x8088C0D0); /* Nord8 highlight */
    comp_draw_string(px + 15 + 15 * 9, py + 82 + 3 * 16, "27", 0x002E3440, 0); /* dark text */

    comp_fill_rect(px + 15, py + 172, pw - 30, 1, NORD3 & 0x00FFFFFF);

    /* --- SECTION 2: AUDIO VOLUME SLIDER --- */
    comp_draw_string(px + 15, py + 182, "Volume Control", NORD4 & 0x00FFFFFF, 0);

    /* Progress bar track */
    int32_t sx = px + 15;
    int32_t sy = py + 204;
    int32_t sw_slider = pw - 30;
    int32_t sh_slider = 12;
    comp_draw_rounded_rect(sx, sy, sw_slider, sh_slider, 6, NORD1 & 0x00FFFFFF);

    /* Volume level (0 to 100) */
    extern int g_sys_volume;
    int32_t fill_w = (sw_slider * g_sys_volume) / 100;
    if (fill_w > 0) {
        comp_draw_rounded_rect(sx, sy, fill_w, sh_slider, 6, NORD8 & 0x00FFFFFF);
    }
    
    /* Display text */
    char vol_str[16] = "Vol: ";
    char vol_num[8];
    extern void uint_to_str(uint64_t val, char *buf);
    uint_to_str((uint64_t)g_sys_volume, vol_num);
    wm_str_cat(vol_str, vol_num);
    wm_str_cat(vol_str, "%");
    comp_draw_string(px + 190, py + 182, vol_str, NORD6 & 0x00FFFFFF, 0);

    comp_fill_rect(px + 15, py + 232, pw - 30, 1, NORD3 & 0x00FFFFFF);

    /* --- SECTION 3: SOUND MELODIES --- */
    comp_draw_string(px + 15, py + 242, "Sound Test", NORD4 & 0x00FFFFFF, 0);

    /* Buttons */
    comp_draw_rounded_rect(px + 15, py + 264, 75, 24, 4, NORD2 & 0x00FFFFFF);
    comp_draw_string(px + 28, py + 268, "Chime", NORD6 & 0x00FFFFFF, 0);

    comp_draw_rounded_rect(px + 100, py + 264, 75, 24, 4, NORD2 & 0x00FFFFFF);
    comp_draw_string(px + 118, py + 268, "Beep", NORD6 & 0x00FFFFFF, 0);

    comp_draw_rounded_rect(px + 185, py + 264, 80, 24, 4, NORD2 & 0x00FFFFFF);
    comp_draw_string(px + 208, py + 268, "Mute", NORD6 & 0x00FFFFFF, 0);

    comp_fill_rect(px + 15, py + 304, pw - 30, 1, NORD3 & 0x00FFFFFF);

    /* --- SECTION 4: POWER OPTIONS --- */
    comp_draw_rounded_rect(px + 15, py + 318, 120, 26, 4, 0x80BF616A); /* red */
    comp_draw_string(px + 40, py + 323, "Shut Down", NORD6 & 0x00FFFFFF, 0);

    comp_draw_rounded_rect(px + 145, py + 318, 120, 26, 4, 0x80D08770); /* orange */
    comp_draw_string(px + 185, py + 323, "Reboot", NORD6 & 0x00FFFFFF, 0);
}

extern "C" void wm_draw_desktop(void)
{
    static uint64_t last_sec = 999999;
    uint64_t total_sec = timer_get_ticks() / 100;

    int32_t mx = mouse_get_x();
    int32_t my = mouse_get_y();
    uint8_t buttons = mouse_get_buttons();

    static int32_t last_mx = -1;
    static int32_t last_my = -1;
    static uint8_t last_buttons = 0;

    bool dragging = (g_dragged_window != NULL || g_resizing_window != NULL);

    /* Tick window scale animations using Spring Physics */
    bool any_animating = false;
    Window *curr = g_windows_head;
    while (curr) {
        /* Spring Constants: stiffness = 40, damping = 12 (scaled by 256) */
        int32_t stiffness = 40;
        int32_t damping = 12;

        int32_t displacement = curr->scale_spring.current - curr->scale_spring.target;
        
        if (displacement != 0 || curr->scale_spring.velocity != 0) {
            int32_t force = (-stiffness * displacement - damping * curr->scale_spring.velocity) / 32;
            curr->scale_spring.velocity += force;
            curr->scale_spring.current += curr->scale_spring.velocity;

            any_animating = true;

            /* Check snap to target */
            int32_t new_disp = curr->scale_spring.current - curr->scale_spring.target;
            int32_t abs_disp = new_disp < 0 ? -new_disp : new_disp;
            int32_t abs_vel = curr->scale_spring.velocity < 0 ? -curr->scale_spring.velocity : curr->scale_spring.velocity;
            
            if (abs_disp < 2 && abs_vel < 2) {
                curr->scale_spring.current = curr->scale_spring.target;
                curr->scale_spring.velocity = 0;
                
                /* If target was 0, hide the window completely */
                if (curr->scale_spring.target == 0) {
                    curr->is_visible = false;
                    curr->anim_direction = 0;
                }
            }
        }
        curr = curr->next;
    }
    if (any_animating) {
        g_wm_needs_redraw = true;
    }

    /* Lock interrupts to make desktop drawing and mouse updates atomic and prevent races */
    uint64_t rflags = save_irq_disable();

    if (!g_wm_needs_redraw) {
        /* Update only the clock area in back buffer if second changed */
        bool clock_changed = false;
        if (total_sec != last_sec) {
            last_sec = total_sec;
            clock_changed = true;

            uint32_t sw = comp_get_width();
            wm_draw_panel_contents(sw, total_sec);
        }

        /* If only the mouse moved, buttons changed, or compositor is dirty, do flip */
        extern bool comp_is_dirty(void);
        if (mx != last_mx || my != last_my || buttons != last_buttons || comp_is_dirty() || clock_changed) {
            bool near_dock = (my >= 650) || (last_my >= 650);
            static bool g_prev_control_panel_visible = false;
            bool ctrl_panel_active = g_control_panel_visible || g_prev_control_panel_visible;

            if (dragging || near_dock || ctrl_panel_active) {
                /* Localized Redraw Path for window dragging/resizing and dock magnification */
                uint32_t sw = comp_get_width();
                uint32_t sh = comp_get_height();

                int32_t ux1 = 999999;
                int32_t uy1 = 999999;
                int32_t ux2 = -999999;
                int32_t uy2 = -999999;

                /* Include Control Center bounds if it was or is active */
                if (ctrl_panel_active) {
                    int32_t cpx = sw - 290;
                    int32_t cpy = THEME_PANEL_HEIGHT + 5;
                    int32_t cpw = 280;
                    int32_t cph = 360;

                    if (cpx < ux1) ux1 = cpx;
                    if (cpy < uy1) uy1 = cpy;
                    if (cpx + cpw > ux2) ux2 = cpx + cpw;
                    if (cpy + cph > uy2) uy2 = cpy + cph;
                }
                g_prev_control_panel_visible = g_control_panel_visible;

                /* Include moving/dragged/resized window's old and new bounds in dirty rect */
                if (dragging) {
                    Window *aw = g_dragged_window ? g_dragged_window : g_resizing_window;
                    if (aw) {
                        int32_t px = aw->prev_x;
                        int32_t py = aw->prev_y;
                        uint32_t pw = aw->prev_w;
                        uint32_t ph = aw->prev_h;

                        if (px - 8 < ux1) ux1 = px - 8;
                        if (py - 8 < uy1) uy1 = py - 8;
                        if (px + (int32_t)pw + 16 > ux2) ux2 = px + (int32_t)pw + 16;
                        if (py + (int32_t)ph + 16 > uy2) uy2 = py + (int32_t)ph + 16;

                        int32_t cx = aw->x;
                        int32_t cy = aw->y;
                        uint32_t cw = aw->w;
                        uint32_t ch = aw->h;

                        if (cx - 8 < ux1) ux1 = cx - 8;
                        if (cy - 8 < uy1) uy1 = cy - 8;
                        if (cx + (int32_t)cw + 16 > ux2) ux2 = cx + (int32_t)cw + 16;
                        if (cy + (int32_t)ch + 16 > uy2) uy2 = cy + (int32_t)ch + 16;
                    }
                }

                /* Include dock bounds */
                uint32_t dock_w = 220;
                uint32_t dock_h = THEME_DOCK_HEIGHT;
                int32_t dock_x = (sw - dock_w) / 2;
                int32_t dock_y = sh - dock_h - 15;

                if (near_dock || dragging) {
                    int32_t dx1 = dock_x - 40;
                    int32_t dy1 = dock_y - 60;
                    int32_t dx2 = dock_x + (int32_t)dock_w + 40;
                    int32_t dy2 = dock_y + (int32_t)dock_h + 20;

                    if (dx1 < ux1) ux1 = dx1;
                    if (dy1 < uy1) uy1 = dy1;
                    if (dx2 > ux2) ux2 = dx2;
                    if (dy2 > uy2) uy2 = dy2;
                }

                /* Include old cursor bounds */
                int32_t sc_x = -1, sc_y = -1;
                bool has_cursor = false;
                extern void comp_get_cursor_save_info(int32_t *x, int32_t *y, bool *has_cursor);
                comp_get_cursor_save_info(&sc_x, &sc_y, &has_cursor);
                if (has_cursor) {
                    if (sc_x < ux1) ux1 = sc_x;
                    if (sc_y < uy1) uy1 = sc_y;
                    if (sc_x + 18 > ux2) ux2 = sc_x + 18;
                    if (sc_y + 19 > uy2) uy2 = sc_y + 19;
                }

                /* Include new cursor bounds */
                if (mx < ux1) ux1 = mx;
                if (my < uy1) uy1 = my;
                if (mx + 18 > ux2) ux2 = mx + 18;
                if (my + 19 > uy2) uy2 = my + 19;

                /* Clamp union to screen */
                if (ux1 < 0) ux1 = 0;
                if (uy1 < 0) uy1 = 0;
                if (ux2 > (int32_t)sw) ux2 = (int32_t)sw;
                if (uy2 > (int32_t)sh) uy2 = (int32_t)sh;

                if (ux1 < ux2 && uy1 < uy2) {
                    /* Restore old background under cursor */
                    comp_restore_cursor_back();

                    /* Restore wallpaper inside the union rect (skip if top window is maximized) */
                    extern void comp_draw_wallpaper_rect(uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh);
                    bool loc_top_max = false;
                    {
                        Window *tw = g_windows_head;
                        while (tw) {
                            if (tw->is_visible) {
                                if (tw->is_maximized) loc_top_max = true;
                                break;
                            }
                            tw = tw->next;
                        }
                    }
                    if (!loc_top_max) {
                        comp_draw_wallpaper_rect(ux1, uy1, ux2 - ux1, uy2 - uy1);
                    }

                    /* Redraw all windows from bottom to top, but only if they intersect the dirty bounds */
                    Window *win = g_windows_tail;
                    while (win) {
                        if (win->is_visible) {
                            int32_t wx1 = win->x;
                            int32_t wy1 = win->y;
                            int32_t wx2 = win->x + (int32_t)win->w;
                            int32_t wy2 = win->y + (int32_t)win->h;

                            /* Include shadow (+4px bottom-right) in intersection check */
                            if (wx1 < ux2 && (wx2 + 4) > ux1 &&
                                wy1 < uy2 && (wy2 + 4) > uy1) {
                                
                                int32_t orig_x = win->x;
                                int32_t orig_y = win->y;
                                uint32_t orig_w = win->w;
                                uint32_t orig_h = win->h;
                                int32_t scale = win->scale_spring.current;

                                 if (scale != 256 || win->scale_spring.velocity != 0) {
                                    win->w = (orig_w * scale) / 256;
                                    win->h = (orig_h * scale) / 256;
                                    win->x = orig_x + (int32_t)(orig_w - win->w) / 2;
                                    win->y = orig_y + (int32_t)(orig_h - win->h) / 2;
                                    if (win->w < 1) win->w = 1;
                                    if (win->h < 1) win->h = 1;
                                }

                                draw_window_decorations(win);
                                if (win->draw_content && scale > 96) {
                                    win->draw_content(win);
                                }
                                if (win->is_focused && !win->is_maximized && scale == 256) {
                                    int32_t rx = win->x + win->w - 12;
                                    int32_t ry = win->y + win->h - 12;
                                    comp_fill_rect(rx, ry, 8, 8, THEME_ACCENT & 0x00FFFFFF);
                                }

                                win->x = orig_x;
                                win->y = orig_y;
                                win->w = orig_w;
                                win->h = orig_h;
                            }
                        }
                        win = win->prev;
                    }

                    /* Wireframe outline rendering removed for real-time dragging */

                    /* Redraw top panel if it overlaps the union rect */
                    if (uy1 < (int32_t)THEME_PANEL_HEIGHT) {
                        wm_draw_panel_contents(sw, total_sec);
                    }

                    /* Redraw dock if near dock or dragging */
                    if (near_dock || dragging) {
                        comp_fill_rect_alpha(dock_x, dock_y, dock_w, dock_h, THEME_DOCK_BG);
                        comp_draw_rounded_rect(dock_x, dock_y, dock_w, dock_h, 12, THEME_DOCK_BG);
                        comp_draw_rounded_rect_border(dock_x, dock_y, dock_w, dock_h, 12, 0x40FFFFFF);

                        int spacing = dock_w / (DOCK_ICON_COUNT + 1);
                        for (int i = 0; i < DOCK_ICON_COUNT; i++) {
                            g_dock_icons[i].cx = dock_x + spacing * (i + 1);
                            g_dock_icons[i].cy = dock_y + dock_h / 2;

                            int32_t dx = mx - g_dock_icons[i].cx;
                            int32_t dy = my - g_dock_icons[i].cy;
                            int32_t dist_sq = dx * dx + dy * dy;

                            if (dist_sq < 80 * 80) {
                                uint32_t zoom = (80 * 80 - dist_sq) / 400;
                                g_dock_icons[i].current_radius = g_dock_icons[i].base_radius + zoom;
                            } else {
                                g_dock_icons[i].current_radius = g_dock_icons[i].base_radius;
                            }

                            draw_dock_icon(i, g_dock_icons[i].cx, g_dock_icons[i].cy, g_dock_icons[i].current_radius);
                            if (i == 0 && g_term_window && g_term_window->is_visible) {
                                comp_draw_circle(g_dock_icons[i].cx, dock_y + dock_h - 4, 2, THEME_ACCENT & 0x00FFFFFF);
                            }
                            if (i == 1 && g_settings_window && g_settings_window->is_visible) {
                                comp_draw_circle(g_dock_icons[i].cx, dock_y + dock_h - 4, 2, THEME_ACCENT & 0x00FFFFFF);
                            }
                            if (i == 2 && g_browser_window && g_browser_window->is_visible) {
                                comp_draw_circle(g_dock_icons[i].cx, dock_y + dock_h - 4, 2, THEME_ACCENT & 0x00FFFFFF);
                            }
                            if (i == 3 && g_forge_window && g_forge_window->is_visible) {
                                comp_draw_circle(g_dock_icons[i].cx, dock_y + dock_h - 4, 2, THEME_ACCENT & 0x00FFFFFF);
                            }
                        }

                        /* Hover Tooltip in Localized Path */
                        int32_t hovered_idx = -1;
                        int32_t min_dist_sq = 999999;
                        for (int k = 0; k < DOCK_ICON_COUNT; k++) {
                            int32_t dx = mx - g_dock_icons[k].cx;
                            int32_t dy = my - g_dock_icons[k].cy;
                            int32_t dist_sq = dx*dx + dy*dy;
                            if (dist_sq < 35 * 35 && dist_sq < min_dist_sq) {
                                min_dist_sq = dist_sq;
                                hovered_idx = k;
                            }
                        }

                        if (hovered_idx != -1) {
                            int k = hovered_idx;
                            int32_t rad = g_dock_icons[k].current_radius;
                            const char *name = g_dock_icons[k].name;
                            uint32_t name_len = 0;
                            while (name[name_len]) name_len++;
                            
                            uint32_t box_w = name_len * 9 + 16;
                            uint32_t box_h = 20;
                            int32_t box_x = g_dock_icons[k].cx - (int32_t)box_w / 2;
                            int32_t box_y = g_dock_icons[k].cy - rad - 24;

                            comp_draw_rounded_rect(box_x, box_y, box_w, box_h, 6, 0xD52E3440);
                            comp_draw_string(box_x + 8, box_y + 2, name, 0xFFECEFF4, 0);
                        }
                    }

                    /* Draw Control Center overlay in localized path if visible */
                    if (g_control_panel_visible) {
                        wm_draw_control_panel(sw, sh);
                    }

                    /* Set compositor dirty region to the union rect */
                    extern void comp_set_dirty_rect(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
                    comp_set_dirty_rect(ux1, uy1, ux2 - 1, uy2 - 1);

                    /* Draw cursor at new position */
                    comp_draw_cursor(mx, my, buttons);

                    /* Flip! */
                    compositor_flip();

                    /* Update previous outline bounds */
                    /* previous outline bounds updates removed */
                }
            } else {
                /* Cursor-only fast path */
                comp_restore_cursor_back();
                comp_draw_cursor(mx, my, buttons);
                compositor_flip();
            }

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

    /* Check if the top-most visible window is maximized — skip wallpaper entirely */
    bool top_maximized = false;
    {
        Window *tw = g_windows_head;
        while (tw) {
            if (tw->is_visible) {
                if (tw->is_maximized) top_maximized = true;
                break;
            }
            tw = tw->next;
        }
    }
    if (!top_maximized) {
        comp_draw_wallpaper();
    }

    /* 2. Draw all windows in reverse Z-order (bottom-most to top-most) */
    Window *win = g_windows_tail;
    while (win) {
        if (win->is_visible) {
            int32_t orig_x = win->x;
            int32_t orig_y = win->y;
            uint32_t orig_w = win->w;
            uint32_t orig_h = win->h;
            int32_t scale = win->scale_spring.current;

            if (scale != 256 || win->scale_spring.velocity != 0) {
                win->w = (orig_w * scale) / 256;
                win->h = (orig_h * scale) / 256;
                win->x = orig_x + (int32_t)(orig_w - win->w) / 2;
                win->y = orig_y + (int32_t)(orig_h - win->h) / 2;
                if (win->w < 1) win->w = 1;
                if (win->h < 1) win->h = 1;
            }

            draw_window_decorations(win);
            if (win->draw_content && scale > 96) {
                win->draw_content(win);
            }
            /* Draw a resize handle in bottom-right corner of window (skip if maximized) */
            if (win->is_focused && !win->is_maximized && scale == 256) {
                int32_t rx = win->x + win->w - 12;
                int32_t ry = win->y + win->h - 12;
                comp_fill_rect(rx, ry, 8, 8, THEME_ACCENT & 0x00FFFFFF);
            }

            win->x = orig_x;
            win->y = orig_y;
            win->w = orig_w;
            win->h = orig_h;
        }
        win = win->prev;
    }

    /* Wireframe outline rendering removed for real-time dragging */

    /* 3. Top panel */
    wm_draw_panel_contents(sw, total_sec);

    /* 4. Draw Dock Panel with Magnification */
    uint32_t dock_w = 220;
    uint32_t dock_h = THEME_DOCK_HEIGHT;
    int32_t dock_x = (sw - dock_w) / 2;
    int32_t dock_y = sh - dock_h - 15;

    /* Blur under Dock bypassed for performance */

    /* Draw semi-transparent dock background */
    comp_fill_rect_alpha(dock_x, dock_y, dock_w, dock_h, THEME_DOCK_BG);
    comp_draw_rounded_rect(dock_x, dock_y, dock_w, dock_h, 12, THEME_DOCK_BG);
    comp_draw_rounded_rect_border(dock_x, dock_y, dock_w, dock_h, 12, 0x40FFFFFF);

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
        draw_dock_icon(i, g_dock_icons[i].cx, g_dock_icons[i].cy, g_dock_icons[i].current_radius);
        
        /* Draw little dot under active app */
        if (i == 0 && g_term_window && g_term_window->is_visible) {
            comp_draw_circle(g_dock_icons[i].cx, dock_y + dock_h - 4, 2, THEME_ACCENT & 0x00FFFFFF);
        }
        if (i == 1 && g_settings_window && g_settings_window->is_visible) {
            comp_draw_circle(g_dock_icons[i].cx, dock_y + dock_h - 4, 2, THEME_ACCENT & 0x00FFFFFF);
        }
        if (i == 2 && g_browser_window && g_browser_window->is_visible) {
            comp_draw_circle(g_dock_icons[i].cx, dock_y + dock_h - 4, 2, THEME_ACCENT & 0x00FFFFFF);
        }
        if (i == 3 && g_forge_window && g_forge_window->is_visible) {
            comp_draw_circle(g_dock_icons[i].cx, dock_y + dock_h - 4, 2, THEME_ACCENT & 0x00FFFFFF);
        }
    }

    /* Hover Tooltip in Full Redraw Path */
    int32_t hovered_idx = -1;
    int32_t min_dist_sq = 999999;
    for (int k = 0; k < DOCK_ICON_COUNT; k++) {
        int32_t dx = mx - g_dock_icons[k].cx;
        int32_t dy = my - g_dock_icons[k].cy;
        int32_t dist_sq = dx*dx + dy*dy;
        if (dist_sq < 35 * 35 && dist_sq < min_dist_sq) {
            min_dist_sq = dist_sq;
            hovered_idx = k;
        }
    }

    if (hovered_idx != -1) {
        int k = hovered_idx;
        int32_t rad = g_dock_icons[k].current_radius;
        const char *name = g_dock_icons[k].name;
        uint32_t name_len = 0;
        while (name[name_len]) name_len++;
        
        uint32_t box_w = name_len * 9 + 16;
        uint32_t box_h = 20;
        int32_t box_x = g_dock_icons[k].cx - (int32_t)box_w / 2;
        int32_t box_y = g_dock_icons[k].cy - rad - 24;

        comp_draw_rounded_rect(box_x, box_y, box_w, box_h, 6, 0xD52E3440);
        comp_draw_string(box_x + 8, box_y + 2, name, 0xFFECEFF4, 0);
    }

    /* Draw Control Center overlay if visible */
    if (g_control_panel_visible) {
        wm_draw_control_panel(sw, sh);
    }

    /* 5. Draw cursor on the back-buffer right before flipping */
    comp_draw_cursor(mx, my, buttons);

    /* Flip onto screen */
    compositor_flip();

    /* Make sure windows have correct initial previous bounds */
    Window *w_curr = g_windows_tail;
    while (w_curr) {
        w_curr->prev_x = w_curr->x;
        w_curr->prev_y = w_curr->y;
        w_curr->prev_w = w_curr->w;
        w_curr->prev_h = w_curr->h;
        w_curr = w_curr->prev;
    }

    last_mx = mx;
    last_my = my;
    last_buttons = buttons;

    restore_irq(rflags);
}

static void wm_show_window(Window *win)
{
    if (win) {
        if (!win->is_visible || win->scale_spring.current < 256) {
            win->is_visible = true;
            win->anim_direction = 1;
            win->scale_spring.target = 256;
            if (win->scale_spring.current == 0) {
                win->scale_spring.velocity = 0;
            }
        }
        wm_raise_window(win);
        comp_mark_dirty();
    }
}

void wm_handle_mouse(int32_t mx, int32_t my, uint8_t buttons)
{
    bool left_pressed  = (buttons & 1) != 0;
    bool prev_left     = (g_prev_buttons & 1) != 0;
    bool clicked_down  = left_pressed && !prev_left;
    bool clicked_up    = !left_pressed && prev_left;

    /* If we have a captured window, forward all mouse events to it first */
    extern Window *g_mouse_captured_window;
    if (g_mouse_captured_window) {
        g_mouse_captured_window->handle_mouse(mx, my, buttons);
        if (clicked_up) {
            g_mouse_captured_window = NULL;
        }
        g_prev_buttons = buttons;
        if (clicked_down || clicked_up) {
            comp_mark_dirty();
        }
        return;
    }

    if (clicked_down) {
        bool handled = false;

        /* 1. Control Center Interactivity */
        if (g_control_panel_visible) {
            uint32_t sw = comp_get_width();
            int32_t px = sw - 290;
            int32_t py = THEME_PANEL_HEIGHT + 5;
            int32_t pw = 280;
            int32_t ph = 360;

            if (mx >= px && mx < px + pw && my >= py && my < py + ph) {
                handled = true;

                /* Volume Slider click: y = py + 204 to py + 216 */
                if (mx >= px + 15 && mx < px + pw - 15 && my >= py + 204 && my < py + 216) {
                    int32_t offset_x = mx - (px + 15);
                    extern int g_sys_volume;
                    g_sys_volume = (offset_x * 100) / 250;
                    if (g_sys_volume < 0) g_sys_volume = 0;
                    if (g_sys_volume > 100) g_sys_volume = 100;
                    
                    /* Feedback beep */
                    extern void beep(uint32_t freq, uint32_t duration_ms);
                    beep(440 + g_sys_volume * 4, 40);
                    comp_mark_dirty();
                }

                /* Chime button click */
                if (mx >= px + 15 && mx < px + 90 && my >= py + 264 && my < py + 288) {
                    extern void play_startup_chime(void);
                    play_startup_chime();
                }

                /* Beep button click */
                if (mx >= px + 100 && mx < px + 175 && my >= py + 264 && my < py + 288) {
                    extern void beep(uint32_t freq, uint32_t duration_ms);
                    beep(800, 100);
                }

                /* Mute button click */
                if (mx >= px + 185 && mx < px + 265 && my >= py + 264 && my < py + 288) {
                    extern void nosound(void);
                    nosound();
                }

                /* Shut Down button click */
                if (mx >= px + 15 && mx < px + 135 && my >= py + 318 && my < py + 344) {
                    extern void sys_poweroff(void);
                    sys_poweroff();
                }

                /* Reboot button click */
                if (mx >= px + 145 && mx < px + 265 && my >= py + 318 && my < py + 344) {
                    extern void sys_reboot(void);
                    sys_reboot();
                }
            } else {
                /* Clicked outside Control Center: close it! */
                g_control_panel_visible = false;
                comp_mark_dirty();
                handled = true; /* consume click */
            }
        }

        /* 2. Check if clicked on top-right clock/status area to open Control Center */
        if (!handled && my < (int32_t)THEME_PANEL_HEIGHT) {
            uint32_t sw = comp_get_width();
            if (mx >= (int32_t)sw - 120) {
                g_control_panel_visible = true;
                comp_mark_dirty();
                handled = true;
            }
        }

        /* 3. Check if clicked on a window */
        Window *win = g_windows_head;
        while (win) {
            if (win->is_visible && win->scale_spring.target == 256) { /* Only interact if open */
                /* Check boundary */
                if (mx >= win->x && mx < (int32_t)(win->x + win->w) &&
                    my >= win->y && my < (int32_t)(win->y + win->h)) {
                    
                    /* Click is inside the window! Raise it to front */
                    wm_raise_window(win);
                    handled = true;                    /* 1. Titlebar click */
                    if (my < (int32_t)(win->y + THEME_TITLEBAR_HEIGHT)) {
                        
                        /* 1a. Close button hit (red, cx=18 cy=14) */
                        int32_t close_dx = mx - (win->x + 18);
                        int32_t close_dy = my - (win->y + 14);
                        if (close_dx * close_dx + close_dy * close_dy <= 6 * 6) {
                            if (win->id == 1) {
                                /* Exit GUI mode if terminal is closed */
                                wm_exit_gui();
                            } else {
                                win->anim_direction = -1; /* Close zoom-out animation */
                                win->scale_spring.target = 0;
                                comp_mark_dirty();
                            }
                            break;
                        }

                        /* 1b. Minimize button hit (yellow, cx=38 cy=14) */
                        int32_t min_dx = mx - (win->x + 38);
                        int32_t min_dy = my - (win->y + 14);
                        if (min_dx * min_dx + min_dy * min_dy <= 6 * 6) {
                            win->anim_direction = -1; /* Minimize zoom-out animation */
                            win->scale_spring.target = 0;
                            comp_mark_dirty();
                            break;
                        }

                        /* 1c. Maximize button hit (green, cx=58 cy=14) */
                        int32_t max_dx = mx - (win->x + 58);
                        int32_t max_dy = my - (win->y + 14);
                        if (max_dx * max_dx + max_dy * max_dy <= 6 * 6) {
                            uint32_t scr_w = comp_get_width();
                            uint32_t scr_h = comp_get_height();
                            if (!win->is_maximized) {
                                /* Save normal floating bounds */
                                win->normal_x = win->x;
                                win->normal_y = win->y;
                                win->normal_w = win->w;
                                win->normal_h = win->h;
                                /* Expand to fill screen below panel */
                                win->x = 0;
                                win->y = THEME_PANEL_HEIGHT;
                                win->w = scr_w;
                                win->h = scr_h - THEME_PANEL_HEIGHT;
                                win->is_maximized = true;
                            } else {
                                /* Restore normal floating bounds */
                                win->x = win->normal_x;
                                win->y = win->normal_y;
                                win->w = win->normal_w;
                                win->h = win->normal_h;
                                win->is_maximized = false;
                            }
                            /* Sync terminal console boundaries if terminal */
                            if (win->id == 1) {
                                console_start_x = win->x + 15;
                                console_start_y = win->y + 40;
                                console_end_x = win->x + win->w - 15;
                                console_end_y = win->y + win->h - 15;
                            }
                            comp_mark_dirty();
                            break;
                        }

                        /* 1d. Dragging titlebar (blocked for maximized windows) */
                        if (!win->is_maximized) {
                            g_dragged_window = win;
                            g_drag_offset_x = mx - win->x;
                            g_drag_offset_y = my - win->y;
                        }
                    }
                    /* 2. Client area click */
                    else {
                        /* 2a. Resize handle in bottom-right corner (blocked for maximized) */
                        if (!win->is_maximized) {
                            int32_t rx = win->x + win->w - 12;
                            int32_t ry = win->y + win->h - 12;
                            if (mx >= rx && mx < (int32_t)(win->x + win->w) &&
                                my >= ry && my < (int32_t)(win->y + win->h)) {
                                g_resizing_window = win;
                                g_resize_start_w = win->w;
                                g_resize_start_h = win->h;
                                g_resize_start_mx = mx;
                                g_resize_start_my = my;
                            } else {
                                /* 2b. Custom window mouse event */
                                win->handle_mouse(mx, my, buttons);
                                g_mouse_captured_window = win;
                            }
                        } else {
                            /* Maximized: only custom mouse, no resize */
                            win->handle_mouse(mx, my, buttons);
                            g_mouse_captured_window = win;
                        }
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
                            wm_show_window(g_term_window);
                        } else if (i == 1 && g_settings_window) {
                            wm_show_window(g_settings_window);
                        } else if (i == 2 && g_browser_window) {
                            wm_show_window(g_browser_window);
                        } else if (i == 3 && g_forge_window) {
                            wm_show_window(g_forge_window);
                        }
                        break;
                    }
                }
            }
        }
    }

    /* Dragging action in real time */
    if (left_pressed && g_dragged_window) {
        int32_t new_x = mx - g_drag_offset_x;
        int32_t new_y = my - g_drag_offset_y;

        /* Clamp y to panel height */
        if (new_y < (int32_t)THEME_PANEL_HEIGHT) {
            new_y = THEME_PANEL_HEIGHT;
        }

        if (g_dragged_window->x != new_x || g_dragged_window->y != new_y) {
            g_dragged_window->x = new_x;
            g_dragged_window->y = new_y;

            /* Synchronize console text boundaries in real time */
            if (g_dragged_window->id == 1) {
                console_start_x = g_dragged_window->x + 15;
                console_start_y = g_dragged_window->y + 40;
                console_end_x = g_dragged_window->x + g_dragged_window->w - 15;
                console_end_y = g_dragged_window->y + g_dragged_window->h - 15;
            }
            comp_mark_dirty();
        }
    }

    /* Resizing action in real time */
    if (left_pressed && g_resizing_window) {
        int32_t dw = mx - g_resize_start_mx;
        int32_t dh = my - g_resize_start_my;

        int32_t new_w = (int32_t)g_resize_start_w + dw;
        int32_t new_h = (int32_t)g_resize_start_h + dh;

        if (new_w < 180) new_w = 180;
        if (new_h < 120) new_h = 120;

        if ((int32_t)g_resizing_window->w != new_w || (int32_t)g_resizing_window->h != new_h) {
            g_resizing_window->w = new_w;
            g_resizing_window->h = new_h;

            /* Synchronize console text boundaries in real time */
            if (g_resizing_window->id == 1) {
                console_start_x = g_resizing_window->x + 15;
                console_start_y = g_resizing_window->y + 40;
                console_end_x = g_resizing_window->x + g_resizing_window->w - 15;
                console_end_y = g_resizing_window->y + g_resizing_window->h - 15;
            }
            comp_mark_dirty();
        }
    }

    /* Release drag/resize locks */
    if (clicked_up) {
        if (g_dragged_window) {
            g_dragged_window = NULL;
            comp_mark_dirty();
        }
        if (g_resizing_window) {
            g_resizing_window = NULL;
            comp_mark_dirty();
        }
        g_mouse_captured_window = NULL;
    }

    g_prev_buttons = buttons;

    /* Forward hover/move events to the window under the mouse */
    if (!left_pressed && !g_dragged_window && !g_resizing_window) {
        Window *hover_win = g_windows_head;
        while (hover_win) {
            if (hover_win->is_visible && hover_win->scale_spring.target == 256) {
                if (mx >= hover_win->x && mx < hover_win->x + (int32_t)hover_win->w &&
                    my >= hover_win->y && my < hover_win->y + (int32_t)hover_win->h) {
                    hover_win->handle_mouse(mx, my, buttons);
                    break;
                }
            }
            hover_win = hover_win->next;
        }
    }

    /* Only trigger a full desktop redraw if something major changed (clicks) */
    if (clicked_down || clicked_up) {
        comp_mark_dirty();
    }
}

extern "C" void wm_handle_key(uint8_t scancode, char ascii)
{
    /* Deliver key presses only to focused window */
    if (g_windows_head && g_windows_head->is_focused && g_windows_head->is_visible) {
        g_windows_head->handle_key(scancode, ascii);
    }
}

/* ============================================================
 * Web Browser Application Implementation
 * ============================================================ */

#if 0
static void draw_browser_content(Window *self)
{
    /* Client area */
    int32_t cx = self->x + 1;
    int32_t cy = self->y + THEME_TITLEBAR_HEIGHT + 1;
    uint32_t cw = self->w - 2;
    uint32_t ch = self->h - THEME_TITLEBAR_HEIGHT - 2;

    /* Fill title/address bar bg */
    comp_fill_rect(cx, cy, cw, 30, NORD1 & 0x00FFFFFF);

    /* Draw URL input box: Nord3 background, white text */
    comp_draw_rounded_rect(cx + 10, cy + 5, cw - 80, 20, 4, NORD3 & 0x00FFFFFF);
    
    /* Draw URL text */
    comp_draw_string(cx + 15, cy + 7, g_browser_url, 0xFFECEFF4, 0);

    /* Draw blinking or simple edit cursor if browser is focused and active */
    if (self->is_focused) {
        uint32_t url_len = 0;
        while (g_browser_url[url_len]) url_len++;
        comp_draw_string(cx + 15 + url_len * 9, cy + 7, "|", THEME_ACCENT & 0x00FFFFFF, 0);
    }

    /* Draw GO button: accent color (blue), white text */
    comp_draw_rounded_rect(cx + cw - 65, cy + 5, 55, 20, 4, NORD8 & 0x00FFFFFF);
    comp_draw_string(cx + cw - 52, cy + 7, "GO", THEME_WINDOW_BG & 0x00FFFFFF, 0);

    /* Draw separator under address bar */
    comp_fill_rect(cx, cy + 30, cw, 1, NORD3 & 0x00FFFFFF);

    /* Main rendering canvas: white background (NORD6) */
    int32_t rx = cx;
    int32_t ry = cy + 31;
    uint32_t rw = cw;
    uint32_t rh = ch - 31;

    comp_fill_rect(rx, ry, rw, rh, 0x00ECEFF4); // solid paper color

    /* If buffer is empty, show homepage */
    if (g_browser_content_buf[0] == '\0') {
        int32_t text_y = ry + 20;
        comp_draw_string(rx + 20, text_y, "WynlandOS Browser v1.0", 0xFF2E3440, 0); text_y += 18;
        comp_draw_string(rx + 20, text_y, "======================", 0xFF2E3440, 0); text_y += 24;
        comp_draw_string(rx + 20, text_y, "Welcome to the web!", 0xFF5E81AC, 0); text_y += 24;
        comp_draw_string(rx + 20, text_y, "The network stack is up and running.", 0xFF2E3440, 0); text_y += 18;
        comp_draw_string(rx + 20, text_y, "Host HTTP Server is accessible at:", 0xFF2E3440, 0); text_y += 18;
        comp_draw_string(rx + 30, text_y, "http://10.0.2.2:8000/", 0xFF81A1C1, 0); text_y += 24;
        comp_draw_string(rx + 20, text_y, "Instructions:", 0xFF2E3440, 0); text_y += 18;
        comp_draw_string(rx + 30, text_y, "- Click inside the URL bar to type", 0xFF2E3440, 0); text_y += 18;
        comp_draw_string(rx + 30, text_y, "- Press Enter or click GO to load", 0xFF2E3440, 0); text_y += 18;
        return;
    }

    /* Simple HTML renderer */
    int32_t curr_x = rx + 15;
    int32_t curr_y = ry + 15;
    bool is_h1 = false;
    bool is_h2 = false;
    bool is_link = false;

    const char *html = g_browser_content_buf;
    while (*html) {
        if (*html == '<') {
            /* Parse HTML tag */
            html++;
            if (*html == '/') {
                html++;
                if (*html == 'h' || *html == 'H') {
                    html++;
                    if (*html == '1') is_h1 = false;
                    else if (*html == '2' || *html == '3') is_h2 = false;
                } else if (*html == 'a' || *html == 'A') {
                    is_link = false;
                }
            } else {
                if (*html == 'h' || *html == 'H') {
                    html++;
                    if (*html == '1') is_h1 = true;
                    else if (*html == '2' || *html == '3') is_h2 = true;
                } else if (*html == 'a' || *html == 'A') {
                    is_link = true;
                } else if ((*html == 'b' || *html == 'B') && (*(html+1) == 'r' || *(html+1) == 'R')) {
                    curr_x = rx + 15;
                    curr_y += 18;
                } else if (*html == 'p' || *html == 'P') {
                    curr_x = rx + 15;
                    curr_y += 24; // paragraph break
                }
            }
            /* Skip until '>' */
            while (*html && *html != '>') {
                html++;
            }
            if (*html == '>') html++;
        } else if (*html == '\r' || *html == '\n') {
            if (*html == '\n') {
                curr_x = rx + 15;
                curr_y += 18;
            }
            html++;
        } else {
            /* Draw normal char */
            uint32_t color = 0xFF2E3440; // Default text color (Nord dark grey)
            if (is_h1) {
                color = 0xFF5E81AC; // Nord Frost blue for H1
            } else if (is_h2) {
                color = 0xFF8FBCBB; // Nord Frost green-blue for H2
            } else if (is_link) {
                color = 0xFF81A1C1; // Nord Frost link blue
            }

            /* Wrap line if out of bounds */
            if (curr_x + 9 > (int32_t)(rx + rw - 15)) {
                curr_x = rx + 15;
                curr_y += 18;
            }

            /* Stop if we go below the window */
            if (curr_y + 16 > (int32_t)(ry + rh - 10)) {
                break;
            }

            comp_draw_char(curr_x, curr_y, *html, color, 0x00ECEFF4);
            curr_x += 9;
            html++;
        }
    }
}

static void handle_browser_key(Window *self, uint8_t scancode, char ascii)
{
    (void)self;
    /* If ascii is printable, append to g_browser_url */
    uint32_t len = 0;
    while (g_browser_url[len]) len++;

    if (ascii >= 32 && ascii <= 126) {
        if (len < sizeof(g_browser_url) - 2) {
            g_browser_url[len] = ascii;
            g_browser_url[len + 1] = '\0';
            comp_mark_dirty();
        }
    } else if (scancode == 0x0E) { /* Backspace */
        if (len > 0) {
            g_browser_url[len - 1] = '\0';
            comp_mark_dirty();
        }
    } else if (scancode == 0x1C) { /* Enter */
        browser_load_page();
    }
}

static void handle_browser_mouse(Window *self, int32_t mx, int32_t my, uint8_t buttons)
{
    bool left_pressed  = (buttons & 1) != 0;
    static bool prev_left = false;
    bool clicked_down  = left_pressed && !prev_left;
    prev_left = left_pressed;

    if (clicked_down) {
        int32_t cx = self->x + 1;
        int32_t cy = self->y + THEME_TITLEBAR_HEIGHT + 1;
        uint32_t cw = self->w - 2;

        /* GO button bounds */
        int32_t btn_x1 = cx + cw - 65;
        int32_t btn_x2 = cx + cw - 10;
        int32_t btn_y1 = cy + 5;
        int32_t btn_y2 = cy + 25;

        if (mx >= btn_x1 && mx <= btn_x2 && my >= btn_y1 && my <= btn_y2) {
            browser_load_page();
        }
    }
}

static void browser_load_page(void)
{
    /* Clear display buffer */
    memset(g_browser_content_buf, 0, sizeof(g_browser_content_buf));
    
    /* Draw connecting... status message */
    int32_t cx = g_browser_window->x + 1;
    int32_t cy = g_browser_window->y + THEME_TITLEBAR_HEIGHT + 1;
    uint32_t cw = g_browser_window->w - 2;
    uint32_t ch = g_browser_window->h - THEME_TITLEBAR_HEIGHT - 2;
    int32_t rx = cx;
    int32_t ry = cy + 31;
    uint32_t rw = cw;
    uint32_t rh = ch - 31;

    comp_fill_rect(rx, ry, rw, rh, 0x00ECEFF4);
    comp_draw_string(rx + 20, ry + 20, "Connecting and loading page...", 0xFFBF616A, 0);
    compositor_flip();

    char host[128] = {0};
    uint16_t port = 80;
    char path[128] = "/";

    /* Parse g_browser_url */
    const char *src = g_browser_url;
    
    /* Skip http:// if present */
    if (src[0] == 'h' && src[1] == 't' && src[2] == 't' && src[3] == 'p') {
        src += 4;
        if (src[0] == 's') src++;
        if (src[0] == ':' && src[1] == '/' && src[2] == '/') {
            src += 3;
        }
    }

    /* Extract host */
    uint32_t hi = 0;
    while (*src && *src != ':' && *src != '/') {
        if (hi < sizeof(host) - 1) {
            host[hi++] = *src;
        }
        src++;
    }
    host[hi] = '\0';

    /* Extract port if present */
    if (*src == ':') {
        src++;
        uint32_t pval = 0;
        while (*src && *src >= '0' && *src <= '9') {
            pval = pval * 10 + (*src - '0');
            src++;
        }
        if (pval > 0) port = (uint16_t)pval;
    }

    /* Extract path if present */
    if (*src == '/') {
        uint32_t pi = 0;
        while (*src && pi < sizeof(path) - 1) {
            path[pi++] = *src++;
        }
        path[pi] = '\0';
    }

    /* Perform HTTP GET */
    HttpResponse resp;
    int bytes_read = http_get(host, port, path, g_browser_content_buf, sizeof(g_browser_content_buf) - 1, &resp);

    if (bytes_read < 0) {
        /* Fallback: try host as IP directly */
        uint32_t ip = net_str_to_ip(host);
        if (ip != 0) {
            bytes_read = http_get_ip(ip, port, host, path, g_browser_content_buf, sizeof(g_browser_content_buf) - 1, &resp);
        }
    }

    if (bytes_read >= 0) {
        g_browser_content_buf[bytes_read] = '\0';
    } else {
        /* Write error HTML into buffer */
        const char *err_msg = "<html><h1>Error: Connection Failed</h1><p>Failed to connect to host or DNS resolution failed.</p><p>Check if host is reachable.</p></html>";
        uint32_t j = 0;
        while (err_msg[j]) {
            g_browser_content_buf[j] = err_msg[j];
            j++;
        }
        g_browser_content_buf[j] = '\0';
    }

    comp_mark_dirty();
}
#endif
