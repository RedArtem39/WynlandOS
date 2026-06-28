/*
 * WynlandOS - Window Manager and GUI main loop
 * ============================================================
 */
#include "wm.h"
#include "opengl.h"
#define DOCK_ICON_COUNT 6

typedef struct {
    char name[16];
    uint32_t color;
    int32_t cx, cy;
    uint32_t base_radius;
    uint32_t current_radius;
} DockIcon;

extern "C" {
#include "compositor.h"
#include "theme.h"
#include <wynland/heap.h>
#include <wynland/sched.h>
#include <wynland/irq.h>
#include <wynland/mouse.h>
#include <wynland/net.h>
#include <wynland/http.h>

void serial_write_string(const char *str);
void uint_to_str(uint64_t val, char *buf);
void uint_to_hex(uint64_t val, char *buf);
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

void wm_init_shell_widgets(void);
void wm_paint_shell_panel(void);
void wm_paint_shell_dock(void);
void wm_paint_shell_control_center(void);
void wm_paint_shell_notification_center(void);
bool wm_is_control_center_active(void);
bool wm_is_notification_center_active(void);
void wm_handle_shell_mouse(int32_t mx, int32_t my, uint8_t buttons);

void desktop_widgets_draw(void);
bool desktop_widgets_check_hover(int32_t mx, int32_t my);
bool desktop_widgets_handle_mouse(int32_t mx, int32_t my, uint8_t buttons);
bool desktop_widgets_is_dragging(void);

void draw_dock_icon(int i, int32_t cx, int32_t cy, int32_t r);
void wm_str_cat(char *dest, const char *src);
void wm_show_window(Window *win);

Window *g_windows_head = NULL; /* Top-most / Focused window */
Window *g_windows_tail = NULL; /* Bottom-most window */

Window *g_term_window = NULL;
Window *g_settings_window = NULL;
Window *g_browser_window = NULL;
Window *g_forge_window = NULL;
Window *g_opengl_window = NULL;
Window *g_monitor_window = NULL;

DockIcon g_dock_icons[DOCK_ICON_COUNT] = {
    { "Terminal", 0x88C0D0, 0, 0, 16, 16 },
    { "Settings", 0xB48EAD, 0, 0, 16, 16 },
    { "Browser",  0xD08770, 0, 0, 16, 16 },
    { "Forge",    0xA3BE8C, 0, 0, 16, 16 },
    { "MiniGL",   0xEBCB8B, 0, 0, 16, 16 },
    { "Monitor",  0xBF616A, 0, 0, 16, 16 }
};

bool g_control_panel_visible = false;
bool g_notification_panel_visible = false;
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
static void draw_opengl_content(Window *self);



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



/* Forward declarations */
static void draw_terminal_content(Window *self);
static void draw_settings_content(Window *self);
static void draw_monitor_content(Window *self);
static void handle_monitor_mouse(Window *self, int32_t mx, int32_t my, uint8_t buttons);
/*
static void draw_browser_content(Window *self);
static void handle_browser_key(Window *self, uint8_t scancode, char ascii);
static void handle_browser_mouse(Window *self, int32_t mx, int32_t my, uint8_t buttons);
static void browser_load_page(void);
*/

extern void comp_draw_icon_forge(int32_t cx, int32_t cy, int32_t r);

extern "C" void draw_dock_icon(int i, int32_t cx, int32_t cy, int32_t r)
{
    if (i == 0) {
        comp_draw_icon_terminal(cx, cy, r);
    } else if (i == 1) {
        comp_draw_icon_settings(cx, cy, r);
    } else if (i == 2) {
        comp_draw_icon_browser(cx, cy, r);
    } else if (i == 3) {
        comp_draw_icon_forge(cx, cy, r);
    } else if (i >= 0 && i < DOCK_ICON_COUNT) {
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
    wm_mark_dirty();
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
    wm_mark_dirty();
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

extern "C" {
void kfree(void *ptr);
void *kmalloc(size_t size);
}

void wm_resize_backing_store(Window *win) {
    if (!win || !win->backing_store) return;
    kfree(win->backing_store);
    
    uint32_t cw = win->w - 2;
    uint32_t ch = win->h - THEME_TITLEBAR_HEIGHT - 2;
    win->backing_store = (uint32_t *)kmalloc(cw * ch * 4);
    if (win->backing_store) {
        memset(win->backing_store, 0, cw * ch * 4);
    }
}

extern "C" void wm_init(void)
{
    serial_write_string("WM: Initializing Stacking Window Manager...\r\n");

    g_windows_head = NULL;
    g_windows_tail = NULL;
    extern bool g_gui_active;
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
    Window *settings = new Window(2, 420, 140, 340, 330, "System Settings");
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
    
    /* Allocate browser backing store for interpreted draw speedups */
    browser->backing_store = (uint32_t *)kmalloc((540 - 2) * (360 - THEME_TITLEBAR_HEIGHT - 2) * 4);
    if (browser->backing_store) {
        memset(browser->backing_store, 0, (540 - 2) * (360 - THEME_TITLEBAR_HEIGHT - 2) * 4);
    }
    
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

    /* Create MiniGL Window (ID 5) */
    Window *opengl = new Window(5, 220, 160, 360, 360, "MiniGL 3D Cube Demo");
    opengl->is_visible = false; /* Starts hidden, open from Dock */
    opengl->is_focused = false;
    opengl->draw_content = draw_opengl_content;
    g_opengl_window = opengl;
    wm_register_window(opengl);

    /* Create System Monitor Window (ID 6) */
    Window *monitor = new Window(6, 150, 150, 480, 280 + THEME_TITLEBAR_HEIGHT, "System Monitor");
    monitor->is_visible = false; /* Starts hidden, open from Dock */
    monitor->is_focused = false;
    monitor->draw_content = draw_monitor_content;
    monitor->handle_mouse_cb = handle_monitor_mouse;
    g_monitor_window = monitor;
    wm_register_window(monitor);

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

    /* Initialize Qt C++ shell widgets */
    wm_init_shell_widgets();

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

static void draw_opengl_content(Window *self)
{
    int32_t cx = self->x + 1;
    int32_t cy = self->y + THEME_TITLEBAR_HEIGHT + 1;
    uint32_t cw = self->w - 2;
    uint32_t ch = self->h - THEME_TITLEBAR_HEIGHT - 2;

    uint32_t *bb = comp_get_backbuffer();
    uint32_t sw = comp_get_width();
    uint32_t sh = comp_get_height();

    glInit(sw, sh, bb);
    glViewport(cx, cy, cw, ch);

    glEnable(GL_DEPTH_TEST);
    glClear(GL_DEPTH_BUFFER_BIT);

    if (self->is_maximized) {
        comp_fill_rect(cx, cy, cw, ch, 0x00000000); /* solid black */
    } else {
        comp_fill_rect_alpha(cx, cy, cw, ch, 0x80000000); /* 50% translucent black */
    }

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0f, (float)cw / (float)ch, 0.1f, 10.0f);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -3.0f);

    static float angle = 0.0f;
    angle += 3.0f;
    if (angle > 360.0f) angle -= 360.0f;

    glRotatef(angle, 1.0f, 1.0f, 0.0f);

    glBegin(GL_QUADS);

    /* Front face (Red) */
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex3f(-0.5f, -0.5f,  0.5f);
    glVertex3f( 0.5f, -0.5f,  0.5f);
    glVertex3f( 0.5f,  0.5f,  0.5f);
    glVertex3f(-0.5f,  0.5f,  0.5f);

    /* Back face (Green) */
    glColor3f(0.0f, 1.0f, 0.0f);
    glVertex3f(-0.5f, -0.5f, -0.5f);
    glVertex3f(-0.5f,  0.5f, -0.5f);
    glVertex3f( 0.5f,  0.5f, -0.5f);
    glVertex3f( 0.5f, -0.5f, -0.5f);

    /* Top face (Blue) */
    glColor3f(0.0f, 0.0f, 1.0f);
    glVertex3f(-0.5f,  0.5f, -0.5f);
    glVertex3f(-0.5f,  0.5f,  0.5f);
    glVertex3f( 0.5f,  0.5f,  0.5f);
    glVertex3f( 0.5f,  0.5f, -0.5f);

    /* Bottom face (Yellow) */
    glColor3f(1.0f, 1.0f, 0.0f);
    glVertex3f(-0.5f, -0.5f, -0.5f);
    glVertex3f( 0.5f, -0.5f, -0.5f);
    glVertex3f( 0.5f, -0.5f,  0.5f);
    glVertex3f(-0.5f, -0.5f,  0.5f);

    /* Right face (Magenta) */
    glColor3f(1.0f, 0.0f, 1.0f);
    glVertex3f( 0.5f, -0.5f, -0.5f);
    glVertex3f( 0.5f,  0.5f, -0.5f);
    glVertex3f( 0.5f,  0.5f,  0.5f);
    glVertex3f( 0.5f, -0.5f,  0.5f);

    /* Left face (Cyan) */
    glColor3f(0.0f, 1.0f, 1.0f);
    glVertex3f(-0.5f, -0.5f, -0.5f);
    glVertex3f(-0.5f, -0.5f,  0.5f);
    glVertex3f(-0.5f,  0.5f,  0.5f);
    glVertex3f(-0.5f,  0.5f, -0.5f);

    glEnd();

    glDeinit();

    comp_mark_dirty();
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

extern "C" void wm_str_cat(char *dest, const char *src)
{
    while (*dest) dest++;
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

static void wm_draw_panel_contents(uint32_t sw, uint32_t total_sec)
{
    (void)sw; (void)total_sec;
    wm_paint_shell_panel();
}



static void wm_draw_control_panel(uint32_t sw, uint32_t sh)
{
    (void)sw; (void)sh;
    wm_paint_shell_control_center();
}

static void wm_draw_snap_preview_if_needed(int32_t mx, int32_t my, bool left_pressed)
{
    if (g_dragged_window && left_pressed) {
        uint32_t scr_w = comp_get_width();
        uint32_t scr_h = comp_get_height();
        if (mx < 25) {
            comp_draw_glass_surface(4, THEME_PANEL_HEIGHT + 4, scr_w / 2 - 8, scr_h - THEME_PANEL_HEIGHT - THEME_DOCK_HEIGHT - 35, 12, 0x300088FF, 0, 0x800088FF);
        } else if (mx > (int32_t)scr_w - 25) {
            comp_draw_glass_surface(scr_w / 2 + 4, THEME_PANEL_HEIGHT + 4, scr_w / 2 - 8, scr_h - THEME_PANEL_HEIGHT - THEME_DOCK_HEIGHT - 35, 12, 0x300088FF, 0, 0x800088FF);
        } else if (my < (int32_t)THEME_PANEL_HEIGHT + 25) {
            comp_draw_glass_surface(4, THEME_PANEL_HEIGHT + 4, scr_w - 8, scr_h - THEME_PANEL_HEIGHT - 8, 12, 0x300088FF, 0, 0x800088FF);
        }
    }
}


extern "C" bool wm_draw_desktop(void)
{
    static uint64_t last_sec = 999999;
    uint64_t total_sec = timer_get_ticks() / 100;

    int32_t mx = mouse_get_x();
    int32_t my = mouse_get_y();
    uint8_t buttons = mouse_get_buttons();
    bool left_pressed = (buttons & 1) != 0;

    static int32_t last_mx = -1;
    static int32_t last_my = -1;
    static uint8_t last_buttons = 0;

    /* Tick window scale animations using Spring Physics under brief interrupt lock */
    bool any_animating = false;
    uint64_t rflags_anim = save_irq_disable();
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
    restore_irq(rflags_anim);

    if (any_animating) {
        g_wm_needs_redraw = true;
    }

    bool clock_changed = false;
    if (total_sec != last_sec) {
        last_sec = total_sec;
        clock_changed = true;
    }

    extern bool comp_is_dirty(void);
    bool desktop_changed = (g_wm_needs_redraw || comp_is_dirty() || clock_changed);
    bool mouse_changed = (mx != last_mx || my != last_my || buttons != last_buttons);

    if (desktop_changed) {
        g_wm_needs_redraw = false;

        uint32_t sw = comp_get_width();
        uint32_t sh = comp_get_height();

        comp_clear_saved_cursor();

        /* 1. Wallpaper and Desktop Widgets */
        bool top_maximized = false;
        uint64_t rflags_max = save_irq_disable();
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
        restore_irq(rflags_max);

        if (!top_maximized) {
            comp_draw_wallpaper();
            desktop_widgets_draw();
        }

        /* 2. Snapshot the visible windows under a brief interrupt lock to prevent list modification races */
        #define MAX_WINDOWS_SNAP 32
        Window *draw_list[MAX_WINDOWS_SNAP];
        int draw_count = 0;

        uint64_t rflags_snap = save_irq_disable();
        Window *win_curr = g_windows_tail;
        while (win_curr && draw_count < MAX_WINDOWS_SNAP) {
            if (win_curr->is_visible) {
                draw_list[draw_count++] = win_curr;
            }
            win_curr = win_curr->prev;
        }
        restore_irq(rflags_snap);

        /* 3. Windows in Z-order (bottom to top) with interrupts enabled */
        for (int i = 0; i < draw_count; i++) {
            Window *win = draw_list[i];
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
                if (win->backing_store) {
                    uint32_t cx = win->x + 1;
                    uint32_t cy = win->y + THEME_TITLEBAR_HEIGHT + 1;
                    uint32_t cw = win->w - 2;
                    uint32_t ch = win->h - THEME_TITLEBAR_HEIGHT - 2;
                    uint32_t bw = comp_get_width();
                    uint32_t *back_buffer = comp_get_backbuffer();
                    for (uint32_t row = 0; row < ch; row++) {
                        memcpy(&back_buffer[(cy + row) * bw + cx], &win->backing_store[row * cw], cw * 4);
                    }
                } else {
                    win->draw_content(win);
                }
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

        /* 4. Panel, Dock & overlays */
        wm_paint_shell_panel();
        wm_draw_panel_contents(sw, total_sec);
        wm_paint_shell_dock();

        if (wm_is_control_center_active()) {
            wm_draw_control_panel(sw, sh);
        }
        if (wm_is_notification_center_active()) {
            wm_paint_shell_notification_center();
        }

        /* 5. Snap preview if needed */
        wm_draw_snap_preview_if_needed(mx, my, left_pressed);

        /* 6. Draw cursor (software) and update hardware cursor */
        extern bool virtio_gpu_is_active(void);
        extern void virtio_gpu_update_cursor(uint32_t resource_id, uint32_t x, uint32_t y);
        
        comp_draw_cursor(mx, my, buttons);
        if (virtio_gpu_is_active()) {
            virtio_gpu_update_cursor(2, mx, my);
        }

        /* 7. Mark full screen dirty and flip */
        comp_set_dirty_rect(0, 0, sw - 1, sh - 1);
        compositor_flip();

        last_mx = mx;
        last_my = my;
        last_buttons = buttons;
    }
    else if (mouse_changed) {
        extern bool virtio_gpu_is_active(void);
        if (virtio_gpu_is_active()) {
            extern void virtio_gpu_update_cursor(uint32_t resource_id, uint32_t x, uint32_t y);
            virtio_gpu_update_cursor(2, mx, my);
        } else {
            /* Optimize: screen content is identical, only cursor moved!
               Simply restore background, draw new cursor, and flip the tiny changed areas. */
            extern void comp_restore_cursor_back(void);
            comp_restore_cursor_back();
            comp_draw_cursor(mx, my, buttons);
            compositor_flip();
        }

        last_mx = mx;
        last_my = my;
        last_buttons = buttons;
    }

    return (desktop_changed || mouse_changed);
}

extern "C" void wm_show_window(Window *win)
{
    if (win) {
        if (!win->is_visible) {
            win->scale_spring.current = 0;
            win->scale_spring.velocity = 0;
        }
        win->is_visible = true;
        win->anim_direction = 0;
        win->scale_spring.target = 256;
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

    uint32_t sw = comp_get_width();
    uint32_t sh = comp_get_height();

    /* 1. Update cursor type */
    g_current_cursor_type = CURSOR_ARROW; // Default arrow

    if (g_resizing_window != NULL) {
        g_current_cursor_type = CURSOR_RESIZE_NWSE;
    } else if (g_dragged_window != NULL) {
        g_current_cursor_type = CURSOR_POINTER;
    } else {
        /* Check if hovering over shell panel or dock */
        bool over_panel = (my < (int32_t)THEME_PANEL_HEIGHT);
        bool over_dock = (my >= (int32_t)(sh - THEME_DOCK_HEIGHT - 15) && 
                          mx >= (int32_t)(sw - 220) / 2 && 
                          mx < (int32_t)(sw + 220) / 2);
        bool over_ctrl = g_control_panel_visible && 
                         (mx >= (int32_t)sw - 290 && mx < (int32_t)sw - 10 && 
                          my >= (int32_t)THEME_PANEL_HEIGHT + 5 && 
                          my < (int32_t)THEME_PANEL_HEIGHT + 365);
        bool over_notif = g_notification_panel_visible && 
                          (mx >= (int32_t)sw - 290 && mx < (int32_t)sw - 10 && 
                           my >= (int32_t)THEME_PANEL_HEIGHT + 5 && 
                           my < (int32_t)sh - 10);

        if (over_panel || over_dock || over_ctrl || over_notif) {
            g_current_cursor_type = CURSOR_POINTER;
        } else {
            /* Check window hover */
            Window *win = g_windows_head;
            while (win) {
                if (win->is_visible && win->scale_spring.target == 256) {
                    if (mx >= win->x && mx < win->x + (int32_t)win->w &&
                        my >= win->y && my < win->y + (int32_t)win->h) {
                        
                        /* Hovering over window */
                        if (my < win->y + (int32_t)THEME_TITLEBAR_HEIGHT) {
                            /* Traffic light close/min/max buttons hover */
                            if (mx >= win->x + 12 && mx <= win->x + 64 && 
                                my >= win->y + 8 && my <= win->y + 20) {
                                g_current_cursor_type = CURSOR_POINTER;
                            }
                        } else if (win->is_focused && !win->is_maximized &&
                                   mx >= win->x + (int32_t)win->w - 12 &&
                                   my >= win->y + (int32_t)win->h - 12) {
                            /* Resize handle hover */
                            g_current_cursor_type = CURSOR_RESIZE_NWSE;
                        } else if (win->cpp_widgets_root) {
                            uint8_t c = static_cast<ui::Widget*>(win->cpp_widgets_root)->get_hover_cursor(mx, my);
                            if (c != 0) {
                                g_current_cursor_type = c;
                            }
                        } else if (win->id == 3) {
                            /* Web Browser URL input field and client areas */
                            int32_t cx = win->x + 1;
                            int32_t cy = win->y + THEME_TITLEBAR_HEIGHT + 1;
                            uint32_t cw = win->w - 2;
                            if (mx >= cx + 10 && mx <= cx + (int32_t)cw - 70 && 
                                my >= cy + 5 && my <= cy + 25) {
                                g_current_cursor_type = CURSOR_TEXT;
                            }
                        } else if (win->id == 1) {
                            /* Terminal window client area */
                            g_current_cursor_type = CURSOR_TEXT;
                        }
                        break;
                    }
                }
                win = win->next;
            }
            
            /* Check desktop widget hover if we didn't hover over any window */
            if (g_current_cursor_type == CURSOR_ARROW) {
                if (desktop_widgets_check_hover(mx, my)) {
                    g_current_cursor_type = CURSOR_POINTER;
                }
            }
        }
    }

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

    /* If we are dragging a desktop widget, forward events directly to it */
    if (desktop_widgets_is_dragging()) {
        desktop_widgets_handle_mouse(mx, my, buttons);
        g_prev_buttons = buttons;
        comp_mark_dirty();
        return;
    }

    if (clicked_down) {
        bool handled = false;

        /* Forward mouse events to the C++ shell widgets */
        uint32_t sw = comp_get_width();
        uint32_t sh = comp_get_height();
        bool click_in_panel = (my < (int32_t)THEME_PANEL_HEIGHT);
        bool click_in_dock = (my >= (int32_t)(sh - THEME_DOCK_HEIGHT - 15) && mx >= (int32_t)(sw - 220) / 2 && mx < (int32_t)(sw + 220) / 2);
        bool click_in_ctrl = g_control_panel_visible && (mx >= (int32_t)sw - 290 && mx < (int32_t)sw - 10 && my >= (int32_t)THEME_PANEL_HEIGHT + 5 && my < (int32_t)THEME_PANEL_HEIGHT + 365);
        bool click_in_notif = g_notification_panel_visible && (mx >= (int32_t)sw - 290 && mx < (int32_t)sw - 10 && my >= (int32_t)THEME_PANEL_HEIGHT + 5 && my < (int32_t)sh - 10);
 
        if (click_in_panel || click_in_dock || click_in_ctrl || click_in_notif) {
            wm_handle_shell_mouse(mx, my, buttons);
            handled = true;
        } else if (g_control_panel_visible) {
            /* Clicked outside Control Center: close it! */
            g_control_panel_visible = false;
            comp_mark_dirty();
            handled = true;
        } else if (g_notification_panel_visible) {
            /* Clicked outside Notification Center: close it! */
            g_notification_panel_visible = false;
            comp_mark_dirty();
            handled = true;
        }

        /* Check if clicked on a window */
        if (!handled) {
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
                                win->scale_spring.target = 0;
                                comp_mark_dirty();
                            }
                            break;
                        }

                        /* 1b. Minimize button hit (yellow, cx=38 cy=14) */
                        int32_t min_dx = mx - (win->x + 38);
                        int32_t min_dy = my - (win->y + 14);
                        if (min_dx * min_dx + min_dy * min_dy <= 6 * 6) {
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
                            /* Reallocate backing store for the new window size */
                            wm_resize_backing_store(win);

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
        if (!handled) {
            desktop_widgets_handle_mouse(mx, my, buttons);
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
            uint32_t scr_w = comp_get_width();
            uint32_t scr_h = comp_get_height();
            if (mx < 25) {
                /* Snap Left */
                g_dragged_window->normal_x = g_dragged_window->x;
                g_dragged_window->normal_y = g_dragged_window->y;
                g_dragged_window->normal_w = g_dragged_window->w;
                g_dragged_window->normal_h = g_dragged_window->h;
                
                g_dragged_window->x = 4;
                g_dragged_window->y = THEME_PANEL_HEIGHT + 4;
                g_dragged_window->w = scr_w / 2 - 8;
                g_dragged_window->h = scr_h - THEME_PANEL_HEIGHT - THEME_DOCK_HEIGHT - 35;
                g_dragged_window->is_maximized = false;
                wm_resize_backing_store(g_dragged_window);
            } else if (mx > (int32_t)scr_w - 25) {
                /* Snap Right */
                g_dragged_window->normal_x = g_dragged_window->x;
                g_dragged_window->normal_y = g_dragged_window->y;
                g_dragged_window->normal_w = g_dragged_window->w;
                g_dragged_window->normal_h = g_dragged_window->h;
                
                g_dragged_window->x = scr_w / 2 + 4;
                g_dragged_window->y = THEME_PANEL_HEIGHT + 4;
                g_dragged_window->w = scr_w / 2 - 8;
                g_dragged_window->h = scr_h - THEME_PANEL_HEIGHT - THEME_DOCK_HEIGHT - 35;
                g_dragged_window->is_maximized = false;
                wm_resize_backing_store(g_dragged_window);
            } else if (my < (int32_t)THEME_PANEL_HEIGHT + 25) {
                /* Maximize */
                g_dragged_window->normal_x = g_dragged_window->x;
                g_dragged_window->normal_y = g_dragged_window->y;
                g_dragged_window->normal_w = g_dragged_window->w;
                g_dragged_window->normal_h = g_dragged_window->h;
                
                g_dragged_window->x = 0;
                g_dragged_window->y = THEME_PANEL_HEIGHT;
                g_dragged_window->w = scr_w;
                g_dragged_window->h = scr_h - THEME_PANEL_HEIGHT;
                g_dragged_window->is_maximized = true;
                wm_resize_backing_store(g_dragged_window);
            }
            
            /* Synchronize console text boundaries in real time */
            if (g_dragged_window->id == 1) {
                console_start_x = g_dragged_window->x + 15;
                console_start_y = g_dragged_window->y + 40;
                console_end_x = g_dragged_window->x + g_dragged_window->w - 15;
                console_end_y = g_dragged_window->y + g_dragged_window->h - 15;
            }
            g_dragged_window = NULL;
            comp_mark_dirty();
        }
        if (g_resizing_window) {
            wm_resize_backing_store(g_resizing_window);
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

/* System Monitor implementation */
static int32_t monitor_selected_tid = -1;
static int cpu_history[50] = {0};
static int history_count = 0;
static int update_tick = 0;

static void str_copy_local(char *dst, const char *src)
{
    while (*src) {
        *dst++ = *src++;
    }
    *dst = '\0';
}

static void str_append_local(char *dst, const char *src)
{
    while (*dst) dst++;
    str_copy_local(dst, src);
}

static void draw_monitor_content(Window *self)
{
    int32_t cx = self->x + 1;
    int32_t cy = self->y + THEME_TITLEBAR_HEIGHT + 1;
    uint32_t cw = self->w - 2;
    uint32_t ch = self->h - THEME_TITLEBAR_HEIGHT - 2;

    comp_fill_rect_alpha(cx, cy, cw, ch, THEME_WINDOW_BG);

    update_tick++;
    if (update_tick >= 10) {
        update_tick = 0;
        int active_threads = 0;
        Thread *t = sched_get_thread_list();
        if (t) {
            Thread *curr = t;
            do {
                if (curr->state == THREAD_STATE_RUNNING || curr->state == THREAD_STATE_READY) {
                    active_threads++;
                }
                curr = curr->next;
            } while (curr != t);
        }
        int load = active_threads * 12;
        if (load < 3) load = 3;
        static uint32_t rand_state = 12345;
        rand_state = rand_state * 1103515245 + 12345;
        int noise = (rand_state / 65536) % 5;
        load += noise;
        if (load > 100) load = 99;
        
        for (int i = 0; i < 49; i++) {
            cpu_history[i] = cpu_history[i + 1];
        }
        cpu_history[49] = load;
        if (history_count < 50) history_count++;
    }

    int32_t gx = cx + 15;
    int32_t gy = cy + 15;
    int32_t gw = 210;
    int32_t gh = 70;
    
    comp_fill_rect(gx, gy, gw, gh, NORD0 & 0x00FFFFFF);
    comp_draw_rounded_rect_border(gx, gy, gw, gh, 0, 0x30FFFFFF);
    
    for (int y_line = 14; y_line < gh; y_line += 14) {
        comp_draw_line(gx, gy + y_line, gx + gw - 1, gy + y_line, 0x15D8DEE9);
    }
    for (int x_line = 35; x_line < gw; x_line += 35) {
        comp_draw_line(gx + x_line, gy, gx + x_line, gy + gh - 1, 0x15D8DEE9);
    }
    
    int current_load = cpu_history[49];
    for (int i = 50 - history_count; i < 49; i++) {
        int x1 = gx + (i * gw / 50);
        int y1 = gy + gh - (cpu_history[i] * gh / 100);
        int x2 = gx + ((i + 1) * gw / 50);
        int y2 = gy + gh - (cpu_history[i + 1] * gh / 100);
        comp_draw_line(x1, y1, x2, y2, 0xFFA3BE8C);
    }
    
    comp_draw_string(gx + 5, gy + 5, "CPU History", 0xFFD8DEE9, 0);
    char cpu_str[32];
    uint_to_str(current_load, cpu_str);
    wm_str_cat(cpu_str, "%");
    comp_draw_string(gx + gw - 40, gy + 5, cpu_str, 0xFFA3BE8C, 0);

    int32_t mx = cx + 245;
    int32_t my = cy + 15;
    
    size_t used = heap_get_used_memory();
    size_t free_mem = heap_get_free_memory();
    size_t total = used + free_mem;
    
    comp_draw_string(mx, my, "System Memory", 0xFFD8DEE9, 0);
    comp_fill_rect(mx, my + 18, 210, 14, NORD0 & 0x00FFFFFF);
    comp_draw_rounded_rect_border(mx, my + 18, 210, 14, 0, 0x30FFFFFF);
    if (total > 0) {
        int bar_w = (int)((used * 208) / total);
        if (bar_w > 208) bar_w = 208;
        comp_fill_rect(mx + 1, my + 19, bar_w, 12, 0xFF81A1C1);
    }
    
    char mem_legend[128];
    char val_buf[32];
    uint_to_str(used / 1024, val_buf);
    str_copy_local(mem_legend, "Used: ");
    str_append_local(mem_legend, val_buf);
    str_append_local(mem_legend, " KB / ");
    uint_to_str(total / 1024, val_buf);
    str_append_local(mem_legend, val_buf);
    str_append_local(mem_legend, " KB");
    comp_draw_string(mx, my + 38, mem_legend, 0xFFE5E9F0, 0);

    int32_t tx = cx + 15;
    int32_t ty = cy + 105;
    int32_t tw = 450;
    int32_t th = 130;
    
    comp_draw_rounded_rect_border(tx, ty, tw, th, 0, 0x30FFFFFF);
    comp_fill_rect(tx + 1, ty + 1, tw - 2, 20, NORD0 & 0x00FFFFFF);
    comp_draw_string(tx + 8, ty + 4, "PID", 0xFF88C0D0, 0);
    comp_draw_string(tx + 60, ty + 4, "STATE", 0xFF88C0D0, 0);
    comp_draw_string(tx + 160, ty + 4, "STACK RSP", 0xFF88C0D0, 0);
    
    Thread *t_list = sched_get_thread_list();
    if (t_list) {
        Thread *curr = t_list;
        int row = 0;
        do {
            int32_t row_y = ty + 21 + row * 18;
            if (row_y + 18 > ty + th) break;
            
            bool is_selected = (monitor_selected_tid == (int32_t)curr->id);
            if (is_selected) {
                comp_fill_rect(tx + 1, row_y, tw - 2, 17, 0xFF434C5E & 0x00FFFFFF);
            } else if (row % 2 == 1) {
                comp_fill_rect(tx + 1, row_y, tw - 2, 17, 0xFF2E3440 & 0x00FFFFFF);
            }
            
            char pid_str[32];
            uint_to_str(curr->id, pid_str);
            comp_draw_string(tx + 8, row_y + 2, pid_str, 0xFFECEFF4, 0);
            
            const char *state_name = "UNKNOWN";
            uint32_t state_color = 0xFFECEFF4;
            switch (curr->state) {
                case THREAD_STATE_READY:
                    state_name = "READY";
                    state_color = 0xFFEBCB8B;
                    break;
                case THREAD_STATE_RUNNING:
                    state_name = "RUNNING";
                    state_color = 0xFFA3BE8C;
                    break;
                case THREAD_STATE_BLOCKED:
                    state_name = "BLOCKED";
                    state_color = 0xFFD08770;
                    break;
                case THREAD_STATE_TERMINATED:
                    state_name = "KILLED";
                    state_color = 0xFFBF616A;
                    break;
            }
            comp_draw_string(tx + 60, row_y + 2, state_name, state_color, 0);
            
            char rsp_str[32];
            uint_to_hex(curr->rsp, rsp_str);
            comp_draw_string(tx + 160, row_y + 2, rsp_str, 0xFFD8DEE9, 0);
            
            row++;
            curr = curr->next;
        } while (curr != t_list);
    }

    int32_t btn_x = cx + 335;
    int32_t btn_y = cy + 245;
    int32_t btn_w = 130;
    int32_t btn_h = 24;
    
    comp_fill_rect(btn_x, btn_y, btn_w, btn_h, 0xFFBF616A & 0x00FFFFFF);
    comp_draw_rounded_rect_border(btn_x, btn_y, btn_w, btn_h, 0, 0xFFECEFF4);
    comp_draw_string(btn_x + 25, btn_y + 5, "KILL TASK", 0xFFECEFF4, 0);
}

static void handle_monitor_mouse(Window *self, int32_t mx, int32_t my, uint8_t buttons)
{
    bool left_pressed = (buttons & 1) != 0;
    static bool prev_left = false;
    bool clicked = left_pressed && !prev_left;
    prev_left = left_pressed;

    if (!clicked) return;

    int32_t cx = self->x + 1;
    int32_t cy = self->y + THEME_TITLEBAR_HEIGHT + 1;

    int32_t tx = cx + 15;
    int32_t ty = cy + 105;
    int32_t tw = 450;
    int32_t th = 130;

    if (mx >= tx && mx < tx + tw && my >= ty + 21 && my < ty + th) {
        int row_index = (my - (ty + 21)) / 18;
        
        Thread *t_list = sched_get_thread_list();
        if (t_list) {
            Thread *curr = t_list;
            int r = 0;
            do {
                if (r == row_index) {
                    monitor_selected_tid = (int32_t)curr->id;
                    break;
                }
                r++;
                curr = curr->next;
            } while (curr != t_list);
        }
        comp_mark_dirty();
    }

    int32_t btn_x = cx + 335;
    int32_t btn_y = cy + 245;
    int32_t btn_w = 130;
    int32_t btn_h = 24;

    if (mx >= btn_x && mx < btn_x + btn_w && my >= btn_y && my < btn_y + btn_h) {
        if (monitor_selected_tid >= 1) {
            sched_kill_thread(monitor_selected_tid);
            monitor_selected_tid = -1;
            comp_mark_dirty();
        }
    }
}
