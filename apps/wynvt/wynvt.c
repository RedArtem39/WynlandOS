// WynlandOS - wynvt: a text console without the desktop (Linux's VT, or
// kmscon). The screen is the framebuffer (/dev/fb0: the GOP one on a real
// machine, virtio-gpu's in QEMU), the keyboard its raw scancodes
// (/dev/input/kbd), and in between a PTY with `login:` and then the
// account's shell. libvterm (as in apps/term) understands what programs
// print; glyphs are the kernel console's 8x16 font.
//
// Root's: the wynrc service "console" (rootfs/etc/wynrc/services/console)
// runs it -- in place of the desktop with console=1 in boot.cfg, or by hand
// once the desktop is stopped (ary rc-service wynlogin stop; ary
// rc-service console start).
#define _GNU_SOURCE                    /* posix_openpt, ptsname, getgrouplist */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/fb.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vterm.h>

#include "accounts.h"
#include "font8x16.h"                 /* include/wynland/font.h, its types left out (the build) */
#include "../../zerp_layouts.h"

#define SYS_fb_flush 406

extern char **environ;

/* ------------------------------------------------------------- screen */

static uint8_t *g_fb;
static uint32_t g_w, g_h, g_pitch;
static int g_rows, g_cols;
static VTerm *g_vt;
static VTermScreen *g_vs;
static VTermPos g_cursor;
static bool g_cursor_on = true;
/* the cells to draw again: a rectangle in cell units (empty: r0 > r1) */
static int g_r0 = 1, g_r1 = 0, g_c0 = 1, g_c1 = 0;

static void dirty(int r0, int c0, int r1, int c1)   /* inclusive */
{
    if (g_r0 > g_r1) { g_r0 = r0; g_r1 = r1; g_c0 = c0; g_c1 = c1; return; }
    if (r0 < g_r0) g_r0 = r0;
    if (r1 > g_r1) g_r1 = r1;
    if (c0 < g_c0) g_c0 = c0;
    if (c1 > g_c1) g_c1 = c1;
}

static uint32_t rgb(VTermColor c)
{
    vterm_screen_convert_color_to_rgb(g_vs, &c);
    return ((uint32_t)c.rgb.red << 16) | ((uint32_t)c.rgb.green << 8) | c.rgb.blue;
}

/* the glyph of a code point in the 8x16 font (Latin-1 range; '?' else) */
static const uint8_t *glyph(uint32_t ch)
{
    if (ch == 0) ch = ' ';
    if (ch > 0xFF) ch = '?';
    return font_8x16[ch];
}

static void draw_cell(int row, int col)
{
    VTermScreenCell cell;
    VTermPos pos = { row, col };
    if (!vterm_screen_get_cell(g_vs, pos, &cell)) return;
    uint32_t fg = rgb(cell.fg), bg = rgb(cell.bg);
    if (cell.attrs.reverse) { uint32_t t = fg; fg = bg; bg = t; }
    if (g_cursor_on && row == g_cursor.row && col == g_cursor.col) { uint32_t t = fg; fg = bg; bg = t; }
    const uint8_t *gl = glyph(cell.chars[0]);
    for (int y = 0; y < 16; y++) {
        uint32_t *px = (uint32_t *)(g_fb + (uint64_t)(row * 16 + y) * g_pitch) + col * 8;
        uint8_t bits = gl[y];
        if (cell.attrs.underline && y == 14) bits = 0xFF;
        for (int x = 0; x < 8; x++) px[x] = (bits & (0x80 >> x)) ? fg : bg;
        if (cell.attrs.bold && fg != bg)      /* a bold glyph: one column wider */
            for (int x = 7; x > 0; x--) if (bits & (0x80 >> (x - 1))) px[x] = fg;
    }
}

static void paint(void)
{
    if (g_r0 > g_r1) return;
    for (int r = g_r0; r <= g_r1 && r < g_rows; r++)
        for (int c = g_c0; c <= g_c1 && c < g_cols; c++) draw_cell(r, c);
    syscall(SYS_fb_flush, (long)g_c0 * 8, (long)g_r0 * 16,
            (long)(g_c1 - g_c0 + 1) * 8, (long)(g_r1 - g_r0 + 1) * 16);
    g_r0 = 1; g_r1 = 0;
}

static int cb_damage(VTermRect r, void *u)
{
    (void)u;
    dirty(r.start_row, r.start_col, r.end_row - 1, r.end_col - 1);
    return 1;
}

static int cb_movecursor(VTermPos pos, VTermPos old, int visible, void *u)
{
    (void)u;
    dirty(old.row, old.col, old.row, old.col);
    dirty(pos.row, pos.col, pos.row, pos.col);
    g_cursor = pos;
    g_cursor_on = visible;
    return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue *val, void *u)
{
    (void)u;
    if (prop == VTERM_PROP_CURSORVISIBLE) {
        g_cursor_on = val->boolean;
        dirty(g_cursor.row, g_cursor.col, g_cursor.row, g_cursor.col);
    }
    return 1;
}

/* ----------------------------------------------------------- the PTY */

static int g_master = -1;
static pid_t g_child = -1;

static void to_pty(const char *s, size_t len, void *u)
{
    (void)u;
    while (len) {
        ssize_t n = write(g_master, s, len);
        if (n <= 0) { if (errno == EINTR) continue; return; }
        s += n;
        len -= (size_t)n;
    }
}

static void say(int fd, const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0 && write(fd, b, (size_t)n) < 0) {}
}

static bool read_line(char *buf, size_t cap)
{
    size_t n = 0;
    char c;
    while (n + 1 < cap) {
        ssize_t r = read(0, &c, 1);
        if (r <= 0) return false;
        if (c == '\n' || c == '\r') break;
        buf[n++] = c;
    }
    buf[n] = 0;
    return true;
}

/* the child, on the PTY: login, then the account's shell (this process,
   become the account) */
static void login_child(void)
{
    char host[64] = "wynland";
    gethostname(host, sizeof host);
    for (;;) {
        char user[64], pass[128];
        say(1, "\r\nWynlandOS %s -- the text console\r\n\r\n%s login: ", "(wynvt)", host);
        if (!read_line(user, sizeof user)) _exit(1);
        if (!user[0]) continue;
        struct termios t, quiet;
        tcgetattr(0, &t);
        quiet = t;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(0, TCSANOW, &quiet);
        say(1, "Password: ");
        bool got = read_line(pass, sizeof pass);
        tcsetattr(0, TCSANOW, &t);
        say(1, "\r\n");
        struct passwd *pw = got ? getpwnam(user) : NULL;
        if (!pw || !acct_check_password(user, pass)) {
            memset(pass, 0, sizeof pass);
            sleep(2);
            say(1, "Login incorrect\r\n");
            continue;
        }
        memset(pass, 0, sizeof pass);

        /* the account's environment */
        static char bufs[8][320];
        char *env[16];
        int n = 0, b = 0;
        char rt[96];
        snprintf(rt, sizeof rt, "/tmp/runtime-%u", pw->pw_uid);
        mkdir(rt, 0700);
        if (chown(rt, pw->pw_uid, pw->pw_gid) < 0) {}
        snprintf(bufs[b], sizeof bufs[b], "HOME=%s", pw->pw_dir);      env[n++] = bufs[b++];
        snprintf(bufs[b], sizeof bufs[b], "USER=%s", pw->pw_name);     env[n++] = bufs[b++];
        snprintf(bufs[b], sizeof bufs[b], "LOGNAME=%s", pw->pw_name);  env[n++] = bufs[b++];
        snprintf(bufs[b], sizeof bufs[b], "SHELL=%s", pw->pw_shell);   env[n++] = bufs[b++];
        snprintf(bufs[b], sizeof bufs[b], "XDG_RUNTIME_DIR=%s", rt);   env[n++] = bufs[b++];
        env[n++] = "PATH=/usr/local/bin:/usr/bin:/usr/sbin:/sbin";
        env[n++] = "TERM=xterm-256color";
        env[n++] = "LANG=C.UTF-8";
        const char *tz = getenv("TZ");
        if (tz) { snprintf(bufs[b], sizeof bufs[b], "TZ=%s", tz); env[n++] = bufs[b++]; }
        env[n] = NULL;

        if (chdir(pw->pw_dir) < 0 && chdir("/") < 0) {}
        const char *shell = pw->pw_shell && pw->pw_shell[0] ? pw->pw_shell : "/usr/bin/fish";
        char *argv[] = { (char *)shell, "-l", NULL };
        /* this process becomes the shell, as login(1) does: the PTY is its
           controlling terminal already (a process spawned anew was in no
           session with it -- fish: "No TTY for interactive shell") */
        gid_t list[32];
        int ng = 32;
        if (getgrouplist(pw->pw_name, pw->pw_gid, list, &ng) < 0) ng = 0;
        if (setgroups((size_t)ng, list) < 0 || setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
            say(1, "could not become %s: %s\r\n", pw->pw_name, strerror(errno));
            _exit(1);
        }
        execve(shell, argv, env);
        say(1, "%s: %s\r\n", shell, strerror(errno));
        _exit(127);
    }
}

static bool start_session(void)
{
    if (g_master >= 0) close(g_master);
    g_master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (g_master < 0 || grantpt(g_master) < 0 || unlockpt(g_master) < 0) return false;
    struct winsize ws = { (unsigned short)g_rows, (unsigned short)g_cols, (unsigned short)g_w, (unsigned short)g_h };
    ioctl(g_master, TIOCSWINSZ, &ws);
    char *slave = ptsname(g_master);
    if (!slave) return false;
    char name[64];
    snprintf(name, sizeof name, "%s", slave);
    g_child = fork();
    if (g_child < 0) return false;
    if (g_child == 0) {
        setsid();
        int s = open(name, O_RDWR);
        if (s < 0) _exit(126);
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        if (s > 2) close(s);
        login_child();
        _exit(0);
    }
    fcntl(g_master, F_SETFL, fcntl(g_master, F_GETFL) | O_NONBLOCK);
    return true;
}

/* ------------------------------------------------------------ keyboard */

static int g_layouts[ZERP_LAYOUT_COUNT] = { ZERP_LAYOUT_US }, g_nlayouts = 1, g_layout;
static bool g_shift, g_ctrl, g_alt, g_caps, g_e0, g_toggle_armed;

/* the machine's layouts (keyboard(1) --system: /etc/wynland/keyboard.conf) */
static void load_layouts(void)
{
    FILE *f = fopen("/etc/wynland/keyboard.conf", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *k = line;
        while (*k == ' ' || *k == '\t') k++;
        if (strncmp(k, "kb_layout", 9)) continue;
        char *v = strchr(k, '=');
        if (!v) continue;
        g_nlayouts = 0;
        for (char *t = strtok(v + 1, ", \t\r\n"); t && g_nlayouts < ZERP_LAYOUT_COUNT; t = strtok(NULL, ", \t\r\n"))
            for (int i = 0; i < ZERP_LAYOUT_COUNT; i++)
                if (!strcmp(t, ZERP_LAYOUT_XKB[i])) g_layouts[g_nlayouts++] = i;
        if (!g_nlayouts) g_layouts[g_nlayouts++] = ZERP_LAYOUT_US;
    }
    fclose(f);
}

static VTermModifier mods(void)
{
    int m = 0;
    if (g_shift) m |= VTERM_MOD_SHIFT;
    if (g_ctrl) m |= VTERM_MOD_CTRL;
    if (g_alt) m |= VTERM_MOD_ALT;
    return (VTermModifier)m;
}

static void key(uint8_t sc)
{
    if (sc == 0xE0) { g_e0 = true; return; }
    bool rel = sc & 0x80, e0 = g_e0;
    int code = sc & 0x7F;
    g_e0 = false;

    bool is_shift = !e0 && (code == 0x2A || code == 0x36), is_alt = code == 0x38;
    if (is_shift) g_shift = !rel;
    if (code == 0x1D) g_ctrl = !rel;
    if (is_alt) g_alt = !rel;
    /* Alt+Shift: the next layout, on letting go with nothing between */
    if (!rel && ((is_alt && g_shift) || (is_shift && g_alt))) g_toggle_armed = true;
    else if (!rel && !is_alt && !is_shift) g_toggle_armed = false;
    if (rel && g_toggle_armed && (is_alt || is_shift)) {
        g_toggle_armed = false;
        if (g_nlayouts > 1) g_layout = (g_layout + 1) % g_nlayouts;
    }
    if (rel) return;
    if (!e0 && code == 0x3A) { g_caps = !g_caps; return; }

    VTermKey k = VTERM_KEY_NONE;
    if (e0) {
        switch (code) {
        case 0x48: k = VTERM_KEY_UP; break;
        case 0x50: k = VTERM_KEY_DOWN; break;
        case 0x4B: k = VTERM_KEY_LEFT; break;
        case 0x4D: k = VTERM_KEY_RIGHT; break;
        case 0x47: k = VTERM_KEY_HOME; break;
        case 0x4F: k = VTERM_KEY_END; break;
        case 0x49: k = VTERM_KEY_PAGEUP; break;
        case 0x51: k = VTERM_KEY_PAGEDOWN; break;
        case 0x52: k = VTERM_KEY_INS; break;
        case 0x53: k = VTERM_KEY_DEL; break;
        case 0x1C: k = VTERM_KEY_KP_ENTER; break;
        default: return;
        }
    } else {
        switch (code) {
        case 0x01: k = VTERM_KEY_ESCAPE; break;
        case 0x0E: k = VTERM_KEY_BACKSPACE; break;
        case 0x0F: k = VTERM_KEY_TAB; break;
        case 0x1C: k = VTERM_KEY_ENTER; break;
        case 0x57: k = VTERM_KEY_FUNCTION(11); break;
        case 0x58: k = VTERM_KEY_FUNCTION(12); break;
        default:
            if (code >= 0x3B && code <= 0x44) k = VTERM_KEY_FUNCTION(code - 0x3B + 1);
        }
    }
    if (k != VTERM_KEY_NONE) { vterm_keyboard_key(g_vt, k, mods()); return; }

    if (code >= 59) return;
    /* Ctrl+letter: the US letter whatever the layout (Ctrl+C) */
    uint16_t us = zerp_key_char(ZERP_LAYOUT_US, code, 0, 0);
    if (g_ctrl && us >= 'a' && us <= 'z') {
        vterm_keyboard_unichar(g_vt, us, (VTermModifier)(mods() & ~VTERM_MOD_SHIFT));
        return;
    }
    uint16_t ch = zerp_key_char(g_layouts[g_layout], code, g_shift, g_caps);
    if (ch >= 0x20) vterm_keyboard_unichar(g_vt, ch, (VTermModifier)(mods() & ~VTERM_MOD_SHIFT));
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    int fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    int kbd = open("/dev/input/kbd", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fb < 0 || kbd < 0) { fprintf(stderr, "wynvt: /dev/fb0 or /dev/input/kbd: %s (root's)\n", strerror(errno)); return 1; }
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(fb, FBIOGET_FSCREENINFO, &fix) < 0 || var.bits_per_pixel != 32) {
        fprintf(stderr, "wynvt: no 32-bit framebuffer\n");
        return 1;
    }
    g_w = var.xres; g_h = var.yres; g_pitch = fix.line_length;
    g_fb = mmap(NULL, (size_t)g_pitch * g_h, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (g_fb == MAP_FAILED) { perror("wynvt: mmap /dev/fb0"); return 1; }
    g_cols = (int)(g_w / 8);
    g_rows = (int)(g_h / 16);
    load_layouts();

    g_vt = vterm_new(g_rows, g_cols);
    vterm_set_utf8(g_vt, 1);
    g_vs = vterm_obtain_screen(g_vt);
    static VTermScreenCallbacks cb;
    cb.damage = cb_damage;
    cb.movecursor = cb_movecursor;
    cb.settermprop = cb_settermprop;
    vterm_screen_set_callbacks(g_vs, &cb, NULL);
    vterm_screen_set_damage_merge(g_vs, VTERM_DAMAGE_SCROLL);
    vterm_screen_enable_altscreen(g_vs, 1);
    vterm_screen_reset(g_vs, 1);
    vterm_output_set_callback(g_vt, to_pty, NULL);
    dirty(0, 0, g_rows - 1, g_cols - 1);

    for (;;) {
        if (g_child <= 0 && !start_session()) { sleep(1); continue; }
        struct pollfd p = { g_master, POLLIN, 0 };
        char buf[4096];
        ssize_t n;
        /* (one read per poll: never a read that could wait) */
        if (poll(&p, 1, 10) > 0 && (p.revents & POLLIN) && (n = read(g_master, buf, sizeof buf)) > 0)
            vterm_input_write(g_vt, buf, (size_t)n);
        uint8_t sc[64];
        while ((n = read(kbd, sc, sizeof sc)) > 0)
            for (ssize_t i = 0; i < n; i++) key(sc[i]);
        vterm_screen_flush_damage(g_vs);
        paint();
        /* the session over (logged out): a new login */
        int st = 0;
        if (g_child > 0 && waitpid(g_child, &st, WNOHANG) == g_child) {
            g_child = -1;
            fprintf(stderr, "wynvt: the session ended (%s %d)\n", WIFEXITED(st) ? "exit" : "signal",
                    WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
            vterm_input_write(g_vt, "\r\n", 2);   /* (the old screen stays: what the shell said last) */
        }
    }
}
