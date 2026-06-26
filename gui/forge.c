/*
 * WynlandOS - Forge File Explorer GUI
 * ============================================================
 */
#include "forge.h"
#include "compositor.h"
#include "theme.h"
#include <wynland/types.h>
#include <wynland/vfs.h>
#include <wynland/heap.h>
#include <wynland/irq.h>

#define FORGE_MAX_ITEMS 128

typedef struct {
    char name[MAX_FILENAME];
    uint32_t size;
    bool is_dir;
} ForgeItem;

static char current_forge_dir[256];
static ForgeItem forge_items[FORGE_MAX_ITEMS];
static uint32_t forge_item_count = 0;
static int32_t selected_index = -1;
static bool forge_initialized = false;

static uint64_t last_click_time = 0;
static int32_t last_clicked_index = -1;

/* File Operations and Text Viewer State */
static bool forge_viewing_file = false;
static char file_view_path[256];
static char file_view_buf[2048];
static int32_t file_view_scroll = 0;
static char g_clipboard_path[256] = {0};
static bool g_clipboard_empty = true;

/* Local string helpers */
static void str_copy_local(char *dst, const char *src)
{
    while (*src) {
        *dst++ = *src++;
    }
    *dst = '\0';
}

static int str_compare_local(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static uint32_t str_len_local(const char *s)
{
    uint32_t len = 0;
    while (*s++) len++;
    return len;
}

static void str_append_local(char *dst, const char *src)
{
    while (*dst) dst++;
    str_copy_local(dst, src);
}

static bool str_ends_with_local(const char *str, const char *suffix)
{
    uint32_t len1 = str_len_local(str);
    uint32_t len2 = str_len_local(suffix);
    if (len1 < len2) return false;
    return str_compare_local(str + len1 - len2, suffix) == 0;
}

/* File operations implementation */
static bool forge_copy_file(const char *src, const char *dst)
{
    VfsFile *sf = vfs_open(src);
    if (!sf) return false;

    VfsFile *df = vfs_open_flags(dst, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (!df) {
        vfs_close(sf);
        return false;
    }

    char buf[512];
    int bytes;
    while ((bytes = vfs_read(sf, buf, sizeof(buf))) > 0) {
        vfs_write(df, buf, bytes);
    }

    vfs_close(sf);
    vfs_close(df);
    return true;
}

static void read_forge_directory(void);

static void forge_create_untitled_file(void)
{
    char path[512];
    int count = 0;
    while (1) {
        str_copy_local(path, current_forge_dir);
        if (str_compare_local(current_forge_dir, "/") != 0) {
            str_append_local(path, "/");
        }
        if (count == 0) {
            str_append_local(path, "untitled.txt");
        } else {
            char num_str[16];
            extern void uint_to_str(uint64_t val, char *buf);
            str_append_local(path, "untitled_");
            uint_to_str(count, num_str);
            str_append_local(path, num_str);
            str_append_local(path, ".txt");
        }
        VfsStat st;
        if (!vfs_stat(path, &st)) {
            vfs_create(path);
            break;
        }
        count++;
    }
    read_forge_directory();
}

static void forge_create_untitled_dir(void)
{
    char path[512];
    int count = 0;
    while (1) {
        str_copy_local(path, current_forge_dir);
        if (str_compare_local(current_forge_dir, "/") != 0) {
            str_append_local(path, "/");
        }
        if (count == 0) {
            str_append_local(path, "untitled_dir");
        } else {
            char num_str[16];
            extern void uint_to_str(uint64_t val, char *buf);
            str_append_local(path, "untitled_dir_");
            uint_to_str(count, num_str);
            str_append_local(path, num_str);
        }
        VfsStat st;
        if (!vfs_stat(path, &st)) {
            vfs_mkdir(path);
            break;
        }
        count++;
    }
    read_forge_directory();
}

static void forge_delete_selected(void)
{
    if (selected_index == -1) return;
    ForgeItem *item = &forge_items[selected_index];
    char path[512];
    str_copy_local(path, current_forge_dir);
    if (str_compare_local(current_forge_dir, "/") != 0) {
        str_append_local(path, "/");
    }
    str_append_local(path, item->name);
    vfs_delete(path);
    selected_index = -1;
    read_forge_directory();
}

static void forge_copy_selected(void)
{
    if (selected_index == -1) return;
    ForgeItem *item = &forge_items[selected_index];
    if (item->is_dir) return; /* Currently only support copying files */
    
    str_copy_local(g_clipboard_path, current_forge_dir);
    if (str_compare_local(current_forge_dir, "/") != 0) {
        str_append_local(g_clipboard_path, "/");
    }
    str_append_local(g_clipboard_path, item->name);
    g_clipboard_empty = false;
}

static void forge_paste(void)
{
    if (g_clipboard_empty) return;
    
    const char *filename = g_clipboard_path;
    int len = str_len_local(g_clipboard_path);
    for (int i = len - 1; i >= 0; i--) {
        if (g_clipboard_path[i] == '/') {
            filename = g_clipboard_path + i + 1;
            break;
        }
    }
    
    char dst_path[512];
    str_copy_local(dst_path, current_forge_dir);
    if (str_compare_local(current_forge_dir, "/") != 0) {
        str_append_local(dst_path, "/");
    }
    str_append_local(dst_path, filename);
    
    VfsStat st;
    if (vfs_stat(dst_path, &st)) {
        str_copy_local(dst_path, current_forge_dir);
        if (str_compare_local(current_forge_dir, "/") != 0) {
            str_append_local(dst_path, "/");
        }
        str_append_local(dst_path, "copy_of_");
        str_append_local(dst_path, filename);
    }
    
    forge_copy_file(g_clipboard_path, dst_path);
    read_forge_directory();
}

static void forge_view_selected(void)
{
    if (selected_index == -1) return;
    ForgeItem *item = &forge_items[selected_index];
    if (item->is_dir) return;
    
    str_copy_local(file_view_path, current_forge_dir);
    if (str_compare_local(current_forge_dir, "/") != 0) {
        str_append_local(file_view_path, "/");
    }
    str_append_local(file_view_path, item->name);
    
    VfsFile *f = vfs_open(file_view_path);
    if (f) {
        int bytes = vfs_read(f, file_view_buf, sizeof(file_view_buf) - 1);
        vfs_close(f);
        if (bytes >= 0) {
            file_view_buf[bytes] = '\0';
        } else {
            file_view_buf[0] = '\0';
        }
        forge_viewing_file = true;
        file_view_scroll = 0;
    }
}

static void forge_readdir_callback(VfsNode *node)
{
    if (forge_item_count >= FORGE_MAX_ITEMS) return;
    
    /* Skip "." and ".." folder links */
    if (str_compare_local(node->name, ".") == 0 || str_compare_local(node->name, "..") == 0) {
        return;
    }
    
    ForgeItem *item = &forge_items[forge_item_count++];
    str_copy_local(item->name, node->name);
    item->size = node->size;
    item->is_dir = node->is_dir;
}

static void read_forge_directory(void)
{
    forge_item_count = 0;
    selected_index = -1;
    vfs_readdir(current_forge_dir, forge_readdir_callback);
}

static void init_forge(void)
{
    str_copy_local(current_forge_dir, "/");
    read_forge_directory();
    forge_initialized = true;
}

static void draw_folder_symbol(int32_t x, int32_t y)
{
    /* Draw a mini folder icon (Nord8 teal-blue: 0xFF88C0D0) */
    uint32_t color = 0x0088C0D0;
    comp_fill_rect(x + 2, y + 2, 5, 2, color);
    comp_fill_rect(x, y + 4, 14, 10, color);
}

static void draw_file_symbol(int32_t x, int32_t y)
{
    /* Draw a mini file icon (Nord6 snow-storm: 0xFFECEFF4) */
    uint32_t color = 0x00ECEFF4;
    comp_fill_rect(x + 2, y + 2, 10, 12, color);
    comp_draw_pixel(x + 10, y + 2, 0x003B4252);
    comp_draw_pixel(x + 11, y + 2, 0x003B4252);
    comp_draw_pixel(x + 11, y + 3, 0x003B4252);
}

static void draw_action_button(int32_t x, int32_t y, uint32_t w, uint32_t h, const char *label, bool enabled)
{
    uint32_t bg = enabled ? 0xFF4C566A : 0xFF2E3440;
    uint32_t fg = enabled ? 0xFFECEFF4 : 0xFF4C566A;
    comp_draw_rounded_rect(x, y, w, h, 4, bg & 0x00FFFFFF);
    comp_draw_rounded_rect_border(x, y, w, h, 4, 0x20FFFFFF);
    
    uint32_t label_len = 0;
    while (label[label_len]) label_len++;
    int32_t tx = x + (int32_t)(w - label_len * 9) / 2;
    if (tx < x + 2) tx = x + 2;
    int32_t ty = y + (int32_t)(h - 16) / 2;
    comp_draw_string(tx, ty, label, fg & 0x00FFFFFF, 0);
}

void draw_forge_content(Window *self)
{
    if (!forge_initialized) {
        init_forge();
    }
    
    int32_t cx = self->x + 1;
    int32_t cy = self->y + THEME_TITLEBAR_HEIGHT + 1;
    uint32_t cw = self->w - 2;
    uint32_t ch = self->h - THEME_TITLEBAR_HEIGHT - 2;

    if (forge_viewing_file) {
        if (self->is_maximized) {
            comp_fill_rect(cx, cy, cw, ch, THEME_WINDOW_BG & 0x00FFFFFF);
        } else {
            comp_fill_rect_alpha(cx, cy, cw, ch, THEME_WINDOW_BG);
        }
        
        /* 1. Header of the file viewer */
        comp_fill_rect(cx, cy, cw, 30, NORD1 & 0x00FFFFFF);
        comp_draw_string(cx + 10, cy + 7, file_view_path, NORD6 & 0x00FFFFFF, 0);
        
        /* Close button [X] at the top right */
        int32_t close_btn_x = cx + cw - 25;
        comp_draw_rounded_rect(close_btn_x, cy + 5, 20, 20, 4, 0x00BF616A); /* Nord11 Red close */
        comp_draw_string(close_btn_x + 6, cy + 7, "X", 0x00ECEFF4, 0);
        
        /* Divider */
        comp_fill_rect(cx, cy + 30, cw, 1, NORD3 & 0x00FFFFFF);
        
        /* 2. Text Content Area */
        int32_t tx = cx + 10;
        int32_t ty = cy + 40;
        int32_t th = ch - 50;
        
        comp_fill_rect_alpha(cx + 5, cy + 35, cw - 10, ch - 40, NORD0 & 0x00FFFFFF);
        
        const char *p = file_view_buf;
        int32_t line_y = ty;
        int32_t line_h = 16;
        int32_t max_lines = th / line_h;
        int32_t line_count = 0;
        
        while (*p && line_count < max_lines) {
            char line_buf[80];
            int li = 0;
            while (*p && *p != '\n' && *p != '\r' && li < 79) {
                line_buf[li++] = *p++;
            }
            line_buf[li] = '\0';
            
            comp_draw_string(tx, line_y, line_buf, NORD4 & 0x00FFFFFF, 0);
            line_y += line_h;
            line_count++;
            
            while (*p == '\n' || *p == '\r') p++;
        }
        return;
    }

    /* Fill client area background (Nord0 with alpha/solid support) */
    if (self->is_maximized) {
        comp_fill_rect(cx, cy, cw, ch, THEME_WINDOW_BG & 0x00FFFFFF);
    } else {
        comp_fill_rect_alpha(cx, cy, cw, ch, THEME_WINDOW_BG);
    }

    /* 1. Header (Address bar and Navigation) */
    comp_fill_rect(cx, cy, cw, 30, NORD1 & 0x00FFFFFF);
    
    /* Up/Back button */
    comp_draw_rounded_rect(cx + 8, cy + 5, 24, 20, 4, NORD3 & 0x00FFFFFF);
    comp_draw_string(cx + 14, cy + 7, "^", NORD6 & 0x00FFFFFF, 0);

    /* Path display box */
    comp_draw_rounded_rect(cx + 40, cy + 5, cw - 50, 20, 4, NORD0 & 0x00FFFFFF);
    comp_draw_string(cx + 48, cy + 7, current_forge_dir, NORD4 & 0x00FFFFFF, 0);

    /* Divider */
    comp_fill_rect(cx, cy + 30, cw, 1, NORD3 & 0x00FFFFFF);

    /* 2. Sidebar Favorites (translucent backdrop) */
    int32_t sidebar_w = 120;
    comp_fill_rect_alpha(cx, cy + 31, sidebar_w, ch - 61, 0x403B4252);
    comp_fill_rect(cx + sidebar_w, cy + 31, 1, ch - 61, NORD3 & 0x00FFFFFF);

    comp_draw_string(cx + 10, cy + 45, "PLACES", NORD3 & 0x00FFFFFF, 0);
    
    /* Sidebar Item 1: / Root */
    comp_draw_string(cx + 15, cy + 65, "/ Root", NORD6 & 0x00FFFFFF, 0);
    /* Sidebar Item 2: /apps */
    comp_draw_string(cx + 15, cy + 85, "/ Apps", NORD6 & 0x00FFFFFF, 0);
    /* Sidebar Item 3: /docs */
    comp_draw_string(cx + 15, cy + 105, "/ Docs", NORD6 & 0x00FFFFFF, 0);

    /* 3. Main directory items list */
    int32_t rx = cx + sidebar_w + 1;
    int32_t ry = cy + 31;
    uint32_t rw = cw - sidebar_w - 1;
    uint32_t rh = ch - 61;

    int32_t row_h = 24;
    int32_t max_rows = rh / row_h;
    
    for (uint32_t i = 0; i < forge_item_count && i < (uint32_t)max_rows; i++) {
        int32_t item_y = ry + i * row_h;
        ForgeItem *item = &forge_items[i];

        /* Draw selection background if this row is selected */
        if ((int32_t)i == selected_index) {
            comp_fill_rect_alpha(rx, item_y, rw, row_h, 0x5088C0D0); /* Nord8 accent select */
        }
        
        /* Draw appropriate icon symbol */
        if (item->is_dir) {
            draw_folder_symbol(rx + 10, item_y + 5);
        } else {
            draw_file_symbol(rx + 10, item_y + 5);
        }

        /* Draw item name string */
        comp_draw_string(rx + 30, item_y + 6, item->name, NORD6 & 0x00FFFFFF, 0);

        /* Draw size information for file entries */
        if (!item->is_dir) {
            char size_str[32];
            extern void uint_to_str(uint64_t val, char *buf);
            uint_to_str(item->size, size_str);
            str_append_local(size_str, " B");
            
            uint32_t slen = str_len_local(size_str);
            comp_draw_string(rx + rw - 15 - slen * 9, item_y + 6, size_str, NORD4 & 0x00FFFFFF, 0);
        }
    }

    /* 4. Bottom status/action bar */
    comp_fill_rect(cx, cy + ch - 30, cw, 1, NORD3 & 0x00FFFFFF);
    comp_fill_rect_alpha(cx, cy + ch - 29, cw, 29, 0x403B4252);
    
    /* Status Text */
    if (selected_index == -1) {
        char status_str[32];
        char count_str[16];
        extern void uint_to_str(uint64_t val, char *buf);
        uint_to_str(forge_item_count, count_str);
        str_copy_local(status_str, count_str);
        str_append_local(status_str, " items");
        comp_draw_string(cx + 8, cy + ch - 21, status_str, NORD4 & 0x00FFFFFF, 0);
    } else {
        ForgeItem *item = &forge_items[selected_index];
        char status_str[64];
        if (item->is_dir) {
            str_copy_local(status_str, "Folder: ");
            str_append_local(status_str, item->name);
        } else {
            str_copy_local(status_str, "File: ");
            str_append_local(status_str, item->name);
        }
        /* Clamp status string to fit left side */
        if (str_len_local(status_str) > 18) {
            status_str[15] = '.';
            status_str[16] = '.';
            status_str[17] = '.';
            status_str[18] = '\0';
        }
        comp_draw_string(cx + 8, cy + ch - 21, status_str, NORD4 & 0x00FFFFFF, 0);
    }

    /* Action Buttons */
    bool has_sel = (selected_index != -1);
    bool is_file = has_sel && !forge_items[selected_index].is_dir;
    
    draw_action_button(cx + cw - 320, cy + ch - 25, 60, 20, "New File", true);
    draw_action_button(cx + cw - 255, cy + ch - 25, 60, 20, "New Dir", true);
    draw_action_button(cx + cw - 190, cy + ch - 25, 40, 20, "Copy", is_file);
    draw_action_button(cx + cw - 145, cy + ch - 25, 45, 20, "Paste", !g_clipboard_empty);
    draw_action_button(cx + cw - 95,  cy + ch - 25, 50, 20, "Delete", has_sel);
    draw_action_button(cx + cw - 40,  cy + ch - 25, 35, 20, "View", is_file);
}

void handle_forge_key(Window *self, uint8_t scancode, char ascii)
{
    (void)self; (void)scancode; (void)ascii;
}

void handle_forge_mouse(Window *self, int32_t mx, int32_t my, uint8_t buttons)
{
    bool left_pressed  = (buttons & 1) != 0;
    static bool prev_left = false;
    bool clicked_down  = left_pressed && !prev_left;
    prev_left = left_pressed;

    if (!clicked_down) return;

    int32_t cx = self->x + 1;
    int32_t cy = self->y + THEME_TITLEBAR_HEIGHT + 1;
    uint32_t cw = self->w - 2;
    uint32_t ch = self->h - THEME_TITLEBAR_HEIGHT - 2;

    if (forge_viewing_file) {
        int32_t close_btn_x = cx + cw - 25;
        if (mx >= close_btn_x && mx <= close_btn_x + 20 && my >= cy + 5 && my <= cy + 25) {
            forge_viewing_file = false;
            comp_mark_dirty();
        }
        return;
    }

    /* Up / Parent Navigation Click */
    if (mx >= cx + 8 && mx <= cx + 32 && my >= cy + 5 && my <= cy + 25) {
        if (str_compare_local(current_forge_dir, "/") != 0) {
            uint32_t len = str_len_local(current_forge_dir);
            if (len > 1) {
                int32_t last_slash = -1;
                for (int32_t i = len - 1; i >= 0; i--) {
                    if (current_forge_dir[i] == '/') {
                        last_slash = i;
                        break;
                    }
                }
                if (last_slash == 0) {
                    current_forge_dir[1] = '\0';
                } else if (last_slash > 0) {
                    current_forge_dir[last_slash] = '\0';
                }
            }
            read_forge_directory();
            comp_mark_dirty();
        }
        return;
    }

    /* Sidebar Navigation Clicks */
    int32_t sidebar_w = 120;
    if (mx >= cx && mx < cx + sidebar_w) {
        if (my >= cy + 60 && my <= cy + 80) {
            str_copy_local(current_forge_dir, "/");
            read_forge_directory();
            comp_mark_dirty();
        }
        else if (my >= cy + 80 && my <= cy + 100) {
            vfs_mkdir("/apps");
            str_copy_local(current_forge_dir, "/apps");
            read_forge_directory();
            comp_mark_dirty();
        }
        else if (my >= cy + 100 && my <= cy + 120) {
            vfs_mkdir("/docs");
            str_copy_local(current_forge_dir, "/docs");
            read_forge_directory();
            comp_mark_dirty();
        }
        return;
    }

    /* Bottom Action Bar Clicks */
    if (my >= (int32_t)(cy + ch - 26) && my <= (int32_t)(cy + ch - 4)) {
        bool has_sel = (selected_index != -1);
        bool is_file = has_sel && !forge_items[selected_index].is_dir;
        
        int32_t s_cw = (int32_t)cw;
        if (mx >= cx + s_cw - 320 && mx <= cx + s_cw - 260) {
            forge_create_untitled_file();
            comp_mark_dirty();
        }
        else if (mx >= cx + s_cw - 255 && mx <= cx + s_cw - 195) {
            forge_create_untitled_dir();
            comp_mark_dirty();
        }
        else if (mx >= cx + s_cw - 190 && mx <= cx + s_cw - 150) {
            if (is_file) forge_copy_selected();
            comp_mark_dirty();
        }
        else if (mx >= cx + s_cw - 145 && mx <= cx + s_cw - 100) {
            if (!g_clipboard_empty) forge_paste();
            comp_mark_dirty();
        }
        else if (mx >= cx + s_cw - 95 && mx <= cx + s_cw - 45) {
            if (has_sel) forge_delete_selected();
            comp_mark_dirty();
        }
        else if (mx >= cx + s_cw - 40 && mx <= cx + s_cw - 5) {
            if (is_file) forge_view_selected();
            comp_mark_dirty();
        }
        return;
    }

    /* Main Area Items Click / Navigation Clicks */
    int32_t rx = cx + sidebar_w + 1;
    int32_t ry = cy + 31;
    uint32_t rw = cw - sidebar_w - 1;
    uint32_t rh = ch - 61;

    if (mx >= rx && mx <= (int32_t)(rx + rw) && my >= ry && my <= (int32_t)(ry + rh)) {
        int32_t row_h = 24;
        int32_t clicked_row = (my - ry) / row_h;
        if (clicked_row >= 0 && clicked_row < (int32_t)forge_item_count) {
            uint64_t current_time = timer_get_ticks();
            
            /* Double-click logic (ticks < 35, approx 350ms) */
            if (clicked_row == last_clicked_index && (current_time - last_click_time < 35)) {
                ForgeItem *item = &forge_items[clicked_row];
                if (item->is_dir) {
                    if (str_compare_local(current_forge_dir, "/") == 0) {
                        str_copy_local(current_forge_dir + 1, item->name);
                    } else {
                        uint32_t len = str_len_local(current_forge_dir);
                        current_forge_dir[len] = '/';
                        str_copy_local(current_forge_dir + len + 1, item->name);
                    }
                    read_forge_directory();
                } else {
                    /* Executing files based on .wbin extension */
                    if (str_ends_with_local(item->name, ".wbin")) {
                        char full_path[512];
                        str_copy_local(full_path, current_forge_dir);
                        if (str_compare_local(current_forge_dir, "/") != 0) {
                            uint32_t len = str_len_local(full_path);
                            full_path[len] = '/';
                            full_path[len + 1] = '\0';
                        }
                        str_append_local(full_path, item->name);
                        
                        extern bool wynvm_run(const char *bin_path);
                        wynvm_run(full_path);
                    } else {
                        /* Non-executable file double click -> view in reader! */
                        forge_view_selected();
                    }
                }
                
                last_clicked_index = -1;
                last_click_time = 0;
            } else {
                /* Single click to select */
                selected_index = clicked_row;
                last_clicked_index = clicked_row;
                last_click_time = current_time;
            }
            comp_mark_dirty();
        }
    }
}
