/*
 * WynlandOS / Zerp - terminal, with built-in commands and inline PNG
 * viewing.
 *
 * Built-in commands, not separate spawned binaries -- mirrors the
 * existing Ring-0 WynlandShell's own architecture (kernel/main.c), which
 * deliberately sidesteps needing new stdout-redirection/dup2 kernel
 * infrastructure this pass (there's no corpus of separate Unix-style
 * command binaries in this OS yet either -- ls/cat/etc. are all built
 * into that same Ring-0 dispatcher).
 *
 * Keyboard: Zerp's existing ZERP_MSG_INPUT_KEY (raw PS/2 scancodes) --
 * translated via the identical scancode_to_ascii_lower[] table
 * kernel/main.c already uses (copied here, not shared: kernel and Zerp
 * compile into separate ELF images).
 *
 * Build (see zerp.c's header for the full non-PIE flag rationale):
 *   x86_64-linux-musl-gcc -nostdlib -static -no-pie -fno-pie \
 *     -mcmodel=large -O2 -Wl,-Ttext-segment=0x3A0000000000 \
 *     -o zerp_term.elf zerp_term.c
 */
#include "zerp_font.h"
#include "zerp_png.h"
#include "zerp_vt100.h"

#define O_WRONLY 1

#define ROW_H (ZERP_FONT_HEIGHT + 2)
#define MAX_LINES 200
#define MAX_LINE_LEN 96
#define BG_COLOR   0xFF0A0A14
#define TEXT_COLOR 0xFFCCCCCC
#define ECHO_COLOR 0xFF66FF99
#define ERR_COLOR  0xFFFF6666
#define PROMPT_COLOR 0xFF66CCFF

static const char SCANCODE_TO_ASCII[59] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

static char g_lines[MAX_LINES][MAX_LINE_LEN];
static int  g_line_count = 0; /* total ever added; ring-indexed mod MAX_LINES */

static char g_input[MAX_LINE_LEN];
static int  g_input_len = 0;
static char g_path[256] = "/";

typedef enum { MODE_NORMAL, MODE_PASSWORD, MODE_PTY } Mode;
static Mode g_mode = MODE_NORMAL;
static char g_password_buf[MAX_LINE_LEN];
static int  g_password_len = 0;

/* Phase 18g: real interactive-child (nano) session state. master/child
   are only meaningful while g_mode == MODE_PTY. */
static int  g_pty_master = -1;
static long g_pty_child = -1;
static int  g_ctrl_held = 0;

struct linux_winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
#define TIOCSWINSZ 0x5414

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void str_copy_n(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void add_line(const char *text, uint32_t color) {
    (void)color; /* color currently fixed per call site via a separate table if needed later */
    int idx = g_line_count % MAX_LINES;
    str_copy_n(g_lines[idx], text, MAX_LINE_LEN);
    g_line_count++;
}

/* Splits text on '\n' and adds each resulting line separately -- used for
   multi-line command output (ls, cat). */
static void add_multiline(const char *text) {
    char buf[MAX_LINE_LEN];
    int bi = 0;
    for (int i = 0; text[i]; i++) {
        if (text[i] == '\n') {
            buf[bi] = '\0';
            add_line(buf, TEXT_COLOR);
            bi = 0;
        } else if (bi < MAX_LINE_LEN - 1) {
            buf[bi++] = text[i];
        }
    }
    if (bi > 0) { buf[bi] = '\0'; add_line(buf, TEXT_COLOR); }
}

static ZerpClient *g_zc; /* set once in zerp_main, used by png rendering helper */
static int g_png_pending = 0;
static uint32_t g_png_w, g_png_h;
static uint32_t g_png_buf[1024 * 768]; /* scratch decode target -- big enough for a full-screen-sized test image, tiles are always smaller than this */

static void redraw(ZerpClient *zc) {
    zerp_fill(zc, BG_COLOR);

    uint32_t rows = zc->tile_h / ROW_H;
    if (rows == 0) return;

    if (g_mode == MODE_PTY) {
        /* Blit the VT100 interpreter's monochrome cell grid instead of the
           scrolling line log -- nano owns the whole tile while active. */
        for (int r = 0; r < g_vt_rows; r++) {
            char line[VT_MAX_COLS + 1];
            int c;
            for (c = 0; c < g_vt_cols && c < VT_MAX_COLS; c++) line[c] = g_vt_cell[r][c];
            line[c] = '\0';
            zerp_draw_string_bg(zc, 2, (uint32_t)r * ROW_H, line, TEXT_COLOR, BG_COLOR);
        }
        zerp_send_damage(zc, 0, 0, zc->tile_w, zc->tile_h);
        return;
    }

    if (g_png_pending) {
        /* Blit the decoded image starting at row 0, clipped to the tile. */
        uint32_t draw_w = g_png_w < zc->tile_w ? g_png_w : zc->tile_w;
        uint32_t draw_h = g_png_h < zc->tile_h ? g_png_h : zc->tile_h;
        for (uint32_t y = 0; y < draw_h; y++) {
            for (uint32_t x = 0; x < draw_w; x++) {
                zc->shm[y * zc->tile_w + x] = g_png_buf[y * g_png_w + x];
            }
        }
        zerp_send_damage(zc, 0, 0, zc->tile_w, zc->tile_h);
        return;
    }

    uint32_t visible = rows - 1; /* last row reserved for input/password */
    int total = g_line_count < MAX_LINES ? g_line_count : MAX_LINES;
    int start = g_line_count > (int)visible ? g_line_count - (int)visible : 0;
    uint32_t row = 0;
    for (int i = start; i < g_line_count && row < visible; i++) {
        int idx = i % MAX_LINES;
        zerp_draw_string_bg(zc, 2, row * ROW_H, g_lines[idx], TEXT_COLOR, BG_COLOR);
        row++;
    }
    (void)total;

    if (g_mode == MODE_PASSWORD) {
        char line[MAX_LINE_LEN] = "Password: ";
        int p = 10;
        for (int i = 0; i < g_password_len && p < MAX_LINE_LEN - 1; i++) line[p++] = '*';
        line[p] = '\0';
        zerp_draw_string_bg(zc, 2, visible * ROW_H, line, PROMPT_COLOR, BG_COLOR);
    } else {
        char line[MAX_LINE_LEN] = "> ";
        int p = 2;
        for (int i = 0; i < g_input_len && p < MAX_LINE_LEN - 1; i++) line[p++] = g_input[i];
        line[p] = '\0';
        zerp_draw_string_bg(zc, 2, visible * ROW_H, line, PROMPT_COLOR, BG_COLOR);
    }

    zerp_send_damage(zc, 0, 0, zc->tile_w, zc->tile_h);
}

/* ---------------- built-in commands ---------------- */

static void cmd_pwd(void) { add_line(g_path, TEXT_COLOR); }

static void path_push(const char *name) {
    int len = zstrlen(g_path);
    if (!(len == 1 && g_path[0] == '/')) g_path[len++] = '/';
    int i = 0;
    while (name[i] && len < (int)sizeof(g_path) - 1) g_path[len++] = name[i++];
    g_path[len] = '\0';
}
static void path_pop(void) {
    int len = zstrlen(g_path);
    if (len <= 1) return;
    len--;
    while (len > 0 && g_path[len] != '/') len--;
    if (len == 0) len = 1;
    g_path[len] = '\0';
}
static void path_set_absolute(const char *p) { str_copy_n(g_path, p, (int)sizeof(g_path)); }

struct linux_dirent64 {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

static void cmd_ls(const char *arg) {
    const char *target = (arg && arg[0]) ? arg : g_path;
    long fd = zopen(target, O_RDONLY);
    if (fd < 0) { add_line("ls: cannot open", ERR_COLOR); return; }
    static uint8_t buf[4096];
    long n = zgetdents64((int)fd, buf, sizeof(buf));
    zclose((int)fd);
    if (n <= 0) { add_line("(empty)", TEXT_COLOR); return; }
    long off = 0;
    while (off < n) {
        struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
        if (d->d_reclen == 0) break;
        if (!(d->d_name[0] == '.' && (d->d_name[1] == '\0' ||
              (d->d_name[1] == '.' && d->d_name[2] == '\0')))) {
            char line[MAX_LINE_LEN];
            const char *prefix = (d->d_type == 4) ? "[DIR] " : "      ";
            int p = 0;
            while (prefix[p]) { line[p] = prefix[p]; p++; }
            str_copy_n(line + p, d->d_name, MAX_LINE_LEN - p);
            add_line(line, TEXT_COLOR);
        }
        off += d->d_reclen;
    }
}

static void cmd_cd(const char *arg) {
    if (!arg || !arg[0]) return;
    if (str_eq(arg, "..")) { path_pop(); return; }
    if (arg[0] == '/') { path_set_absolute(arg); return; }
    path_push(arg);
}

static void cmd_whoami(void) {
    long uid = zgetuid();
    add_line(uid == 0 ? "0 (root)" : "1000", TEXT_COLOR);
}

static void cmd_write(const char *args) {
    /* args = "<path> <text...>" */
    char path[128];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < (int)sizeof(path) - 1) { path[i] = args[i]; i++; }
    path[i] = '\0';
    const char *text = (args[i] == ' ') ? args + i + 1 : "";

    long fd = zopen(path, O_WRONLY);
    if (fd < 0) {
        add_line("write: permission denied (EACCES) or path invalid", ERR_COLOR);
        return;
    }
    zwrite((int)fd, text, zstrlen(text));
    zclose((int)fd);
    add_line("write: ok", TEXT_COLOR);
}

static void cmd_cat(const char *path) {
    if (!path || !path[0]) { add_line("cat: missing path", ERR_COLOR); return; }
    long fd = zopen(path, O_RDONLY);
    if (fd < 0) { add_line("cat: cannot open", ERR_COLOR); return; }

    static uint8_t filebuf[ZERP_PNG_MAX_COMPRESSED];
    long total = 0;
    for (;;) {
        long n = zread((int)fd, filebuf + total, (long)sizeof(filebuf) - total);
        if (n <= 0) break;
        total += n;
        if (total >= (long)sizeof(filebuf)) break;
    }
    zclose((int)fd);

    static const uint8_t png_sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    int is_png = (total >= 8);
    if (is_png) for (int i = 0; i < 8; i++) if (filebuf[i] != png_sig[i]) { is_png = 0; break; }

    if (is_png) {
        ZerpPngInfo info = zerp_png_decode(filebuf, (uint32_t)total, g_png_buf, 1024u * 768u);
        if (!info.ok) {
            add_line("cat: PNG decode failed (unsupported format or too large)", ERR_COLOR);
            return;
        }
        g_png_w = info.width;
        g_png_h = info.height;
        g_png_pending = 1;
        return;
    }

    filebuf[total < (long)sizeof(filebuf) ? total : (long)sizeof(filebuf) - 1] = '\0';
    add_multiline((const char *)filebuf);
}

/* Phase 18g: spawn a real interactive nano child on a real PTY, using the
   standard fork()+dup2()+execve() terminal-emulator idiom -- the same
   pattern every real terminal emulator uses to run a program with its
   stdio wired to a PTY slave. */
static void cmd_nano(const char *path) {
    if (!path || !path[0]) { add_line("nano: missing filename", ERR_COLOR); return; }

    int fds[2];
    if (zpty_create(fds) != 0) { add_line("nano: pty_create failed", ERR_COLOR); return; }
    int master = fds[0], slave = fds[1];

    struct linux_winsize ws;
    ws.ws_row = (unsigned short)(g_zc->tile_h / ROW_H);
    ws.ws_col = (unsigned short)(g_zc->tile_w / ZERP_FONT_WIDTH);
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    zioctl(master, TIOCSWINSZ, &ws);

    char pathbuf[128];
    str_copy_n(pathbuf, path, sizeof(pathbuf));

    long pid = zfork();
    if (pid == 0) {
        zdup2(slave, 0);
        zdup2(slave, 1);
        zdup2(slave, 2);
        zclose(master);
        zclose(slave);
        const char *argv2[3];
        argv2[0] = "/usr/bin/nano";
        argv2[1] = pathbuf;
        argv2[2] = 0;
        const char *envp2[2];
        envp2[0] = "TERM=vt100";
        envp2[1] = 0;
        zexecve("/usr/bin/nano", argv2, envp2);
        zexit(1);
    }

    zclose(slave);
    g_pty_master = master;
    g_pty_child = pid;
    g_ctrl_held = 0;
    vt_reset((int)ws.ws_row, (int)ws.ws_col);
    g_mode = MODE_PTY;
}

static void run_command(const char *line) {
    add_line(line, ECHO_COLOR); /* echo what was typed, styled differently from output */

    char cmd[32];
    int i = 0;
    while (line[i] && line[i] != ' ' && i < (int)sizeof(cmd) - 1) { cmd[i] = line[i]; i++; }
    cmd[i] = '\0';
    while (line[i] == ' ') i++;
    const char *args = line + i;

    if (str_eq(cmd, "")) return;
    else if (str_eq(cmd, "ls")) cmd_ls(args);
    else if (str_eq(cmd, "cd")) cmd_cd(args);
    else if (str_eq(cmd, "pwd")) cmd_pwd();
    else if (str_eq(cmd, "cat")) cmd_cat(args);
    else if (str_eq(cmd, "whoami")) cmd_whoami();
    else if (str_eq(cmd, "write")) cmd_write(args);
    else if (str_eq(cmd, "nano")) cmd_nano(args);
    else if (str_eq(cmd, "clear")) { g_line_count = 0; g_png_pending = 0; }
    else if (str_eq(cmd, "sudo")) {
        g_mode = MODE_PASSWORD;
        g_password_len = 0;
    } else if (str_eq(cmd, "rofi")) {
        /* Phase 9 v0 launcher invocation path: request Zerp spawn the real
           Qt6 rofi-clone -- no global-hotkey infrastructure in Zerp yet. */
        zerp_send_spawn(g_zc, "/zerp_rofi.elf");
        add_line("launching rofi...", TEXT_COLOR);
    } else if (str_eq(cmd, "help")) {
        add_line("ls cd pwd cat whoami write nano sudo rofi clear help", TEXT_COLOR);
    } else {
        add_line("unknown command (try 'help')", ERR_COLOR);
    }
}

static void handle_key(uint8_t scancode) {
    /* Left Ctrl (0x1D press / 0x9D release) maps to 0 in SCANCODE_TO_ASCII
       (it's not a printable key) -- intercept it here, before the table
       lookup below, purely to track modifier state for MODE_PTY. */
    if (scancode == 0x1D) { g_ctrl_held = 1; return; }
    if (scancode == 0x9D) { g_ctrl_held = 0; return; }
    if (scancode & 0x80) return; /* key release, ignore */
    if (scancode >= 59) return;
    char ch = SCANCODE_TO_ASCII[scancode];
    if (ch == 0) return;

    if (g_mode == MODE_PTY) {
        /* Raw byte-forwarding to the child's PTY slave -- no line
           buffering, nano requested raw mode itself. Real terminal Ctrl+
           letter semantics: the control byte is the letter's position in
           the alphabet (Ctrl+A=0x01 .. Ctrl+Z=0x1A), i.e. ch & 0x1F. */
        uint8_t out = (uint8_t)ch;
        if (g_ctrl_held && ch >= 'a' && ch <= 'z') out = (uint8_t)(ch - 'a' + 1);
        zwrite(g_pty_master, &out, 1);
        return;
    }

    g_png_pending = 0; /* any keypress dismisses an image view, back to text */

    if (g_mode == MODE_PASSWORD) {
        if (ch == '\n' || ch == '\r') {
            g_password_buf[g_password_len] = '\0';
            long ok = zelevate(g_password_buf);
            add_line(ok == 0 ? "sudo: elevated to root" : "sudo: incorrect password",
                     ok == 0 ? TEXT_COLOR : ERR_COLOR);
            g_mode = MODE_NORMAL;
            g_password_len = 0;
        } else if (ch == '\b') {
            if (g_password_len > 0) g_password_len--;
        } else if (g_password_len < MAX_LINE_LEN - 1) {
            g_password_buf[g_password_len++] = ch;
        }
        return;
    }

    if (ch == '\n' || ch == '\r') {
        g_input[g_input_len] = '\0';
        run_command(g_input);
        g_input_len = 0;
    } else if (ch == '\b') {
        if (g_input_len > 0) g_input_len--;
    } else if (g_input_len < MAX_LINE_LEN - 1) {
        g_input[g_input_len++] = ch;
    }
}

int zerp_main(int argc, char **argv) {
    ZerpClient zc;
    if (zerp_connect(argc, argv, &zc, 2048u * 2048u * 4u) != 0) return 1;
    if (zerp_wait_for_tile(&zc, 100000) != 0) return 1;
    g_zc = &zc;

    add_line("Zerp terminal -- type 'help' for commands", TEXT_COLOR);
    redraw(&zc);

    for (;;) {
        ZerpMsg msg;
        int dirty = 0;
        while (zerp_poll_server_msg(&zc, &msg)) {
            if (msg.type == ZERP_MSG_TILE_RECT) {
                dirty = 1;
            } else if (msg.type == ZERP_MSG_INPUT_KEY) {
                handle_key((uint8_t)msg.x);
                dirty = 1;
            }
        }

        if (g_mode == MODE_PTY) {
            /* Non-blocking drain of the child's screen output into the
               VT100 interpreter, every frame. */
            uint8_t buf[512];
            long n = zread(g_pty_master, buf, sizeof(buf));
            while (n > 0) {
                for (long i = 0; i < n; i++) vt_feed_byte(buf[i]);
                dirty = 1;
                n = zread(g_pty_master, buf, sizeof(buf));
            }
            if (zprocess_alive(g_pty_child) == 0) {
                zclose(g_pty_master);
                g_pty_master = -1;
                g_pty_child = -1;
                g_mode = MODE_NORMAL;
                add_line("(nano exited)", TEXT_COLOR);
                dirty = 1;
            }
        }

        if (dirty) redraw(&zc);
        zyield();
    }
}

#include "zerp_entry.h"
