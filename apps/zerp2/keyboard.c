// WynlandOS - keyboard: the keyboard layouts, from the terminal.
//   keyboard                     the layouts on, the one in use, the ones there are
//   keyboard add ru              one more (Alt+Shift switches between them)
//   keyboard remove ru           one less (English stays: the first can't go)
//   keyboard switch alt-shift    the switch: alt-shift or super-space
//   option: --system (the machine's: /etc, the login screen's too; as root)
//
// "kb_layout = us,ru" and "kb_options = grp:..." in ~/.config/wynland/
// keyboard.conf (or /etc/wynland/keyboard.conf), which Zerp 2.0 reads
// (apps/zerp2/keybinds.h); in a session it is told to read them again
// at once (Hyprland IPC "reload"). The layouts are zerp_layouts.h's.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../zerp_layouts.h"

static int g_layouts[ZERP_LAYOUT_COUNT], g_n;
static int g_superspace;

static int layout_of(const char *name)
{
    for (int i = 0; i < ZERP_LAYOUT_COUNT; i++)
        if (!strcmp(name, ZERP_LAYOUT_XKB[i])) return i;
    return -1;
}

static void conf_path(char *buf, size_t cap, int system)
{
    if (system) { snprintf(buf, cap, "/etc/wynland/keyboard.conf"); return; }
    const char *home = getenv("HOME");
    snprintf(buf, cap, "%s/.config/wynland/keyboard.conf", home ? home : "/tmp");
}

// the settings from a keyboard.conf; false: no such file
static int read_conf(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        if (!strncmp(k, "input:", 6)) k += 6;
        if (!strncmp(k, "kb_layout", 9)) {
            int n = 0;
            for (char *t = strtok(v, ", \t\r\n"); t; t = strtok(NULL, ", \t\r\n")) {
                int l = layout_of(t), dup = 0;
                for (int i = 0; i < n; i++) dup |= g_layouts[i] == l;
                if (l >= 0 && !dup && n < ZERP_LAYOUT_COUNT) g_layouts[n++] = l;
            }
            if (n) g_n = n;
        } else if (!strncmp(k, "kb_options", 10)) {
            g_superspace = strstr(v, "grp:win_space_toggle") != NULL;
        }
    }
    fclose(f);
    return 1;
}

static int mkdirs(const char *path)
{
    char d[512];
    snprintf(d, sizeof d, "%s", path);
    for (char *p = d + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(d, 0755) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return 0;
}

static int write_conf(const char *path)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = mkdirs(path) < 0 ? NULL : fopen(tmp, "w");
    if (!f) { fprintf(stderr, "keyboard: %s: %s\n", path, strerror(errno)); return -1; }
    fprintf(f, "# WynlandOS keyboard layouts (keyboard(1)); Zerp 2.0 reads them\nkb_layout = ");
    for (int i = 0; i < g_n; i++) fprintf(f, "%s%s", i ? "," : "", ZERP_LAYOUT_XKB[g_layouts[i]]);
    fprintf(f, "\nkb_options = %s\n", g_superspace ? "grp:win_space_toggle" : "grp:alt_shift_toggle");
    if (fclose(f) != 0 || rename(tmp, path) < 0) {
        fprintf(stderr, "keyboard: %s: %s\n", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

// a request to Zerp 2.0's Hyprland IPC; its answer into `out`; false: no session
static int ipc(const char *req, char *out, size_t cap)
{
    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE"), *rt = getenv("XDG_RUNTIME_DIR");
    if (!sig || !rt) return 0;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s/hypr/%s/.socket.sock", rt, sig);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0 || write(fd, req, strlen(req)) != (ssize_t)strlen(req)) { close(fd); return 0; }
    size_t n = 0;
    ssize_t r;
    while (n + 1 < cap && (r = read(fd, out + n, cap - 1 - n)) > 0) n += (size_t)r;
    out[n] = 0;
    close(fd);
    return 1;
}

static void show(void)
{
    printf("layouts:   ");
    for (int i = 0; i < g_n; i++) printf("%s%s", i ? ", " : "", ZERP_LAYOUT_XKB[g_layouts[i]]);
    printf("\nswitch:    %s\n", g_n < 2 ? "(one layout: nothing to switch)" : g_superspace ? "Super+Space" : "Alt+Shift");
    char buf[4096];
    const char *k;
    if (ipc("j/devices", buf, sizeof buf) && (k = strstr(buf, "\"active_keymap\":\""))) {
        k += 17;
        const char *e = strchr(k, '"');
        if (e) printf("in use:    %.*s\n", (int)(e - k), k);
    }
    printf("there are: ");
    for (int i = 0; i < ZERP_LAYOUT_COUNT; i++) printf("%s%s (%s)", i ? ", " : "", ZERP_LAYOUT_XKB[i], ZERP_LAYOUT_NAME[i]);
    printf("\n");
}

static void usage(void)
{
    fprintf(stderr,
            "usage: keyboard                        the layouts and the ones there are\n"
            "       keyboard add LAYOUT...          e.g. keyboard add ru\n"
            "       keyboard remove LAYOUT...\n"
            "       keyboard switch alt-shift|super-space\n"
            "option: --system  the machine's (the login screen's too; root)\n");
}

int main(int argc, char **argv)
{
    int system = 0, a = 1;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "--system")) system = 1;
        else if (!strcmp(argv[a], "-h") || !strcmp(argv[a], "--help")) { usage(); return 0; }
        else { usage(); return 2; }
    }
    g_layouts[0] = ZERP_LAYOUT_US;
    g_n = 1;
    char user[512], sys[512];
    conf_path(user, sizeof user, 0);
    conf_path(sys, sizeof sys, 1);
    // what is on: the user's file, else the machine's
    if (system || !read_conf(user)) read_conf(sys);

    if (a >= argc || !strcmp(argv[a], "list")) { show(); return 0; }
    const char *cmd = argv[a++];
    if (!strcmp(cmd, "add") || !strcmp(cmd, "remove")) {
        if (a >= argc) { usage(); return 2; }
        for (; a < argc; a++) {
            int l = layout_of(argv[a]), at = -1;
            if (l < 0) { fprintf(stderr, "keyboard: no layout \"%s\" (keyboard: the ones there are)\n", argv[a]); return 1; }
            for (int i = 0; i < g_n; i++) if (g_layouts[i] == l) at = i;
            if (cmd[0] == 'a') {
                if (at < 0) g_layouts[g_n++] = l;
            } else if (at == 0) {
                fprintf(stderr, "keyboard: %s is the first layout, it stays\n", argv[a]);
                return 1;
            } else if (at > 0) {
                memmove(&g_layouts[at], &g_layouts[at + 1], (size_t)(g_n - at - 1) * sizeof g_layouts[0]);
                g_n--;
            }
        }
    } else if (!strcmp(cmd, "switch")) {
        if (a >= argc) { usage(); return 2; }
        if (!strcmp(argv[a], "alt-shift")) g_superspace = 0;
        else if (!strcmp(argv[a], "super-space")) g_superspace = 1;
        else { usage(); return 2; }
    } else { usage(); return 2; }

    if (system && geteuid() != 0) { fprintf(stderr, "keyboard: --system needs root (ary)\n"); return 1; }
    if (write_conf(system ? sys : user) < 0) return 1;
    char buf[64];
    if (!system && ipc("reload", buf, sizeof buf)) printf("applied\n");
    else printf("applies at the next login\n");
    show();
    return 0;
}
