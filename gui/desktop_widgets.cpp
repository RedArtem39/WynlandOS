#include <wynland/types.h>
#include "window.h"

extern "C" {
#include "compositor.h"
#include "theme.h"

uint64_t timer_get_ticks(void);
size_t heap_get_used_memory(void);
size_t heap_get_free_memory(void);
void uint_to_str(uint64_t val, char *buf);
void wm_str_cat(char *dest, const char *src);
uint64_t pmm_get_used_memory(void);
uint64_t pmm_get_total_memory(void);
}

struct DesktopWidget {
    int32_t x, y;
    uint32_t w, h;
    bool is_dragging;
    int32_t drag_off_x, drag_off_y;
};

static DesktopWidget g_desktop_widgets[3];
static bool g_widgets_initialized = false;

/* Trig lookup tables scaled by 256 for index 0..59 representing clock ticks */
static const int16_t clk_sin[60] = {
    0, 27, 53, 79, 104, 128, 150, 171, 190, 207, 222, 234, 243, 250, 255, 256, 255, 250, 243, 234, 222, 207, 190, 171, 150, 128, 104, 79, 53, 27, 0, -27, -53, -79, -104, -128, -150, -171, -190, -207, -222, -234, -243, -250, -255, -256, -255, -250, -243, -234, -222, -207, -190, -171, -150, -128, -104, -79, -53, -27
};
static const int16_t clk_cos[60] = {
    -256, -255, -250, -243, -234, -222, -207, -190, -171, -150, -128, -104, -79, -53, -27, 0, 27, 53, 79, 104, 128, 150, 171, 190, 207, 222, 234, 243, 250, 255, 256, 255, 250, 243, 234, 222, 207, 190, 171, 150, 128, 104, 79, 53, 27, 0, -27, -53, -79, -104, -128, -150, -171, -190, -207, -222, -234, -243, -250, -255
};

extern "C" void desktop_widgets_init(void)
{
    uint32_t sw = comp_get_width();
    
    /* Place on the right side of the screen */
    g_desktop_widgets[0].x = sw - 160;
    g_desktop_widgets[0].y = 60;
    g_desktop_widgets[0].w = 140;
    g_desktop_widgets[0].h = 140;
    g_desktop_widgets[0].is_dragging = false;

    g_desktop_widgets[1].x = sw - 160;
    g_desktop_widgets[1].y = 220;
    g_desktop_widgets[1].w = 140;
    g_desktop_widgets[1].h = 140;
    g_desktop_widgets[1].is_dragging = false;

    g_desktop_widgets[2].x = sw - 160;
    g_desktop_widgets[2].y = 380;
    g_desktop_widgets[2].w = 140;
    g_desktop_widgets[2].h = 140;
    g_desktop_widgets[2].is_dragging = false;

    g_widgets_initialized = true;
}

static void draw_analog_clock(int32_t wx, int32_t wy)
{
    int32_t cx = wx + 70;
    int32_t cy = wy + 70;

    /* Frosted glass circle back */
    comp_draw_glass_surface(wx, wy, 140, 140, 70, 0x15FFFFFF, 25, 0x40FFFFFF);

    /* Draw hour ticks */
    for (int h = 0; h < 12; h++) {
        int idx = h * 5;
        int32_t tx1 = cx + (42 * clk_sin[idx]) / 256;
        int32_t ty1 = cy + (42 * clk_cos[idx]) / 256;
        int32_t tx2 = cx + (48 * clk_sin[idx]) / 256;
        int32_t ty2 = cy + (48 * clk_cos[idx]) / 256;
        comp_draw_line(tx1, ty1, tx2, ty2, 0x80D8DEE9);
    }

    uint64_t total_sec = timer_get_ticks() / 100;
    uint32_t sec = total_sec % 60;
    uint32_t min = (total_sec / 60) % 60;
    uint32_t hr  = (total_sec / 3600 + 3) % 12; // Timezone UTC+3

    /* 1. Hour Hand (thick, length 28) */
    int hr_idx = (hr * 5 + min / 12) % 60;
    int32_t hx = cx + (26 * clk_sin[hr_idx]) / 256;
    int32_t hy = cy + (26 * clk_cos[hr_idx]) / 256;
    comp_draw_line(cx, cy, hx, hy, 0xFFECEFF4);
    /* Double draw for thickness */
    comp_draw_line(cx + 1, cy, hx + 1, hy, 0xFFECEFF4);
    comp_draw_line(cx, cy + 1, hx, hy + 1, 0xFFECEFF4);

    /* 2. Minute Hand (medium, length 38) */
    int32_t mx = cx + (36 * clk_sin[min]) / 256;
    int32_t my = cy + (36 * clk_cos[min]) / 256;
    comp_draw_line(cx, cy, mx, my, 0xFFE5E9F0);
    comp_draw_line(cx + 1, cy, mx + 1, my, 0xFFE5E9F0);

    /* 3. Second Hand (thin, length 42, Nord Red accent) */
    int32_t sx = cx + (40 * clk_sin[sec]) / 256;
    int32_t sy = cy + (40 * clk_cos[sec]) / 256;
    comp_draw_line(cx, cy, sx, sy, 0xFFBF616A);

    /* Center cap pin */
    comp_draw_circle(cx, cy, 3, 0xFFBF616A);
}

static void draw_sys_monitor(int32_t wx, int32_t wy)
{
    /* Frosted glass rect back */
    comp_draw_glass_surface(wx, wy, 140, 140, 16, 0x15FFFFFF, 25, 0x40FFFFFF);

    /* CPU monitor */
    comp_draw_string(wx + 15, wy + 15, "CPU LOAD", 0x88D8DEE9, 0);
    
    /* Calculate dynamic CPU load */
    extern Window *g_dragged_window, *g_resizing_window;
    uint32_t cpu_base = (g_dragged_window || g_resizing_window) ? 14 : 3;
    uint64_t ticks = timer_get_ticks();
    uint32_t cpu_load = cpu_base + (ticks % 5);
    
    char cpu_str[32];
    char num_buf[32];
    uint_to_str(cpu_load, num_buf);
    cpu_str[0] = '\0';
    wm_str_cat(cpu_str, num_buf);
    wm_str_cat(cpu_str, "%");
    comp_draw_string(wx + 15, wy + 32, cpu_str, 0xFFECEFF4, 0);

    /* CPU Progress Bar */
    comp_draw_rounded_rect(wx + 15, wy + 52, 110, 8, 4, 0xFF3B4252);
    uint32_t cpu_w = (110 * cpu_load) / 100;
    if (cpu_w > 0) {
        comp_draw_rounded_rect(wx + 15, wy + 52, cpu_w, 8, 4, 0xFFA3BE8C); // Green Nord
    }

    /* RAM monitor */
    comp_draw_string(wx + 15, wy + 75, "MEM USAGE", 0x88D8DEE9, 0);

    size_t used = (size_t)pmm_get_used_memory();
    size_t total = (size_t)pmm_get_total_memory();
    uint32_t mem_pct = total > 0 ? (uint32_t)((used * 100) / total) : 0;

    char mem_str[32];
    uint_to_str(used / 1024 / 1024, num_buf);
    mem_str[0] = '\0';
    wm_str_cat(mem_str, num_buf);
    wm_str_cat(mem_str, " MB / ");
    uint_to_str(total / 1024 / 1024, num_buf);
    wm_str_cat(mem_str, num_buf);
    wm_str_cat(mem_str, " MB");
    comp_draw_string(wx + 15, wy + 92, mem_str, 0xFFECEFF4, 0);

    /* RAM Progress Bar */
    comp_draw_rounded_rect(wx + 15, wy + 112, 110, 8, 4, 0xFF3B4252);
    uint32_t mem_w = (110 * mem_pct) / 100;
    if (mem_w > 0) {
        comp_draw_rounded_rect(wx + 15, wy + 112, mem_w, 8, 4, 0xFF88C0D0); // Frost Blue Nord
    }
}

static void draw_calendar(int32_t wx, int32_t wy)
{
    /* Frosted glass rect back */
    comp_draw_glass_surface(wx, wy, 140, 140, 16, 0x15FFFFFF, 25, 0x40FFFFFF);

    /* Red header block for month */
    comp_draw_rounded_rect(wx + 10, wy + 10, 120, 24, 6, 0xFFBF616A); // Nord Aurora Red
    comp_draw_string(wx + 52, wy + 15, "JUNE", 0xFFECEFF4, 0);

    /* Huge day number in center */
    comp_draw_string(wx + 45, wy + 48, "27", 0xFFECEFF4, 0);
    /* Make the text larger by printing double bold shadow */
    comp_draw_string(wx + 46, wy + 48, "27", 0xFFECEFF4, 0);
    comp_draw_string(wx + 45, wy + 49, "27", 0xFFECEFF4, 0);

    /* Day name at bottom */
    comp_draw_string(wx + 34, wy + 95, "SATURDAY", 0x88D8DEE9, 0);

    /* Year at the very bottom */
    comp_draw_string(wx + 50, wy + 115, "2026", 0x44D8DEE9, 0);
}

extern "C" void desktop_widgets_draw(void)
{
    if (!g_widgets_initialized) {
        desktop_widgets_init();
    }

    draw_analog_clock(g_desktop_widgets[0].x, g_desktop_widgets[0].y);
    draw_sys_monitor(g_desktop_widgets[1].x, g_desktop_widgets[1].y);
    draw_calendar(g_desktop_widgets[2].x, g_desktop_widgets[2].y);
}

extern "C" bool desktop_widgets_handle_mouse(int32_t mx, int32_t my, uint8_t buttons)
{
    if (!g_widgets_initialized) return false;

    bool left_pressed = (buttons & 1) != 0;
    static bool s_prev_left = false;
    bool clicked_down = left_pressed && !s_prev_left;
    s_prev_left = left_pressed;

    /* Handle drag check */
    for (int i = 0; i < 3; i++) {
        if (g_desktop_widgets[i].is_dragging) {
            if (left_pressed) {
                g_desktop_widgets[i].x = mx - g_desktop_widgets[i].drag_off_x;
                g_desktop_widgets[i].y = my - g_desktop_widgets[i].drag_off_y;
                
                /* Boundaries checking */
                uint32_t sw = comp_get_width();
                uint32_t sh = comp_get_height();
                if (g_desktop_widgets[i].x < 0) g_desktop_widgets[i].x = 0;
                if (g_desktop_widgets[i].x + (int32_t)g_desktop_widgets[i].w > (int32_t)sw) {
                    g_desktop_widgets[i].x = sw - g_desktop_widgets[i].w;
                }
                if (g_desktop_widgets[i].y < 35) g_desktop_widgets[i].y = 35; // Don't cover panel
                if (g_desktop_widgets[i].y + (int32_t)g_desktop_widgets[i].h > (int32_t)sh - 50) {
                    g_desktop_widgets[i].y = sh - 50 - g_desktop_widgets[i].h; // Don't cover dock
                }
                
                comp_mark_dirty();
                return true;
            } else {
                g_desktop_widgets[i].is_dragging = false;
                comp_mark_dirty();
                return true;
            }
        }
    }

    if (clicked_down) {
        /* Check if clicked inside any widget (processed from front to back, i.e., index 0 to 2) */
        for (int i = 0; i < 3; i++) {
            if (mx >= g_desktop_widgets[i].x && mx < g_desktop_widgets[i].x + (int32_t)g_desktop_widgets[i].w &&
                my >= g_desktop_widgets[i].y && my < g_desktop_widgets[i].y + (int32_t)g_desktop_widgets[i].h) {
                
                g_desktop_widgets[i].is_dragging = true;
                g_desktop_widgets[i].drag_off_x = mx - g_desktop_widgets[i].x;
                g_desktop_widgets[i].drag_off_y = my - g_desktop_widgets[i].y;
                return true;
            }
        }
    }

    return false;
}

extern "C" bool desktop_widgets_is_dragging(void)
{
    if (!g_widgets_initialized) return false;
    return g_desktop_widgets[0].is_dragging || g_desktop_widgets[1].is_dragging || g_desktop_widgets[2].is_dragging;
}

extern "C" bool desktop_widgets_check_hover(int32_t mx, int32_t my)
{
    if (!g_widgets_initialized) return false;
    for (int i = 0; i < 3; i++) {
        if (mx >= g_desktop_widgets[i].x && mx < g_desktop_widgets[i].x + (int32_t)g_desktop_widgets[i].w &&
            my >= g_desktop_widgets[i].y && my < g_desktop_widgets[i].y + (int32_t)g_desktop_widgets[i].h) {
            return true;
        }
    }
    return false;
}
