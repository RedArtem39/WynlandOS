// WynlandOS - resolution: the screen mode, from the terminal.
//   resolution                  the mode now, the one chosen, the ones there are
//   resolution 1920x1080        choose it and restart the desktop (asks first)
//   resolution reset            back to the boot size
//   options: -y (do not ask), --later (at the next login), --system (the
//   machine's default, the login screen's too: /etc, as root)
//
// The modes are the display connector's (DRM, /dev/dri/card0: drivers/
// video/virtgpu_drm.c). The choice is "mode = WxH" in ~/.config/wynland/
// display.conf (or /etc/wynland/display.conf), which Zerp 2.0 reads when
// it starts (apps/zerp2/main.cpp): Qt's eglfs sets a mode only then, so a
// new one means the desktop starts again -- its windows close. Zerp's
// Hyprland IPC "dispatch restartsession" asks for that; wynlogin starts
// the session again without the password (apps/accounts/wynlogin.c).
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// DRM's mode-setting structures (linux/drm_mode.h)
struct mode_info {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
};
struct card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};
struct get_connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id, connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height, subpixel, pad;
};
struct crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct mode_info mode;
};
#define IOCTL_GETRESOURCES _IOWR('d', 0xA0, struct card_res)
#define IOCTL_GETCRTC      _IOWR('d', 0xA1, struct crtc)
#define IOCTL_GETCONNECTOR _IOWR('d', 0xA7, struct get_connector)

#define MAX_MODES 64
#define PREFERRED 0x08   // DRM_MODE_TYPE_PREFERRED

static struct mode_info g_modes[MAX_MODES];
static int g_nmodes;
static unsigned g_cur_w, g_cur_h;

// the connector's modes and the size scanned out now; false: no display driver
static int read_modes(void)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return 0;
    uint32_t crtc_id = 0, conn_id = 0;
    struct card_res r;
    memset(&r, 0, sizeof r);
    r.crtc_id_ptr = (uintptr_t)&crtc_id;
    r.connector_id_ptr = (uintptr_t)&conn_id;
    r.count_crtcs = r.count_connectors = 1;
    if (ioctl(fd, IOCTL_GETRESOURCES, &r) < 0 || !r.count_connectors) { close(fd); return 0; }
    struct get_connector c;
    memset(&c, 0, sizeof c);
    c.connector_id = conn_id;
    c.count_modes = MAX_MODES;
    c.modes_ptr = (uintptr_t)g_modes;
    if (ioctl(fd, IOCTL_GETCONNECTOR, &c) < 0) { close(fd); return 0; }
    g_nmodes = c.count_modes < MAX_MODES ? (int)c.count_modes : MAX_MODES;
    struct crtc k;
    memset(&k, 0, sizeof k);
    k.crtc_id = crtc_id;
    if (ioctl(fd, IOCTL_GETCRTC, &k) == 0 && k.mode_valid) { g_cur_w = k.mode.hdisplay; g_cur_h = k.mode.vdisplay; }
    close(fd);
    return 1;
}

static int find_mode(unsigned w, unsigned h)
{
    for (int i = 0; i < g_nmodes; i++)
        if (g_modes[i].hdisplay == w && g_modes[i].vdisplay == h) return i;
    return -1;
}

static void conf_path(char *buf, size_t cap, int system)
{
    if (system) { snprintf(buf, cap, "/etc/wynland/display.conf"); return; }
    const char *home = getenv("HOME");
    snprintf(buf, cap, "%s/.config/wynland/display.conf", home ? home : "/tmp");
}

// "WxH" from a display.conf: 1; 0 a file without a mode (the boot size,
// as Zerp reads it); -1 no file
static int read_conf(const char *path, unsigned *w, unsigned *h)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    int got = 0;
    while (!got && fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "mode", 4)) continue;
        char *eq = strchr(p, '=');
        if (eq && sscanf(eq + 1, " %ux%u", w, h) == 2 && *w && *h) got = 1;
    }
    fclose(f);
    return got;
}

static int mkdirs(const char *path)   // the directories above `path`
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

// w == 0: no mode (the boot size)
static int write_conf(const char *path, unsigned w, unsigned h)
{
    if (mkdirs(path) < 0) { fprintf(stderr, "resolution: %s: %s\n", path, strerror(errno)); return -1; }
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { fprintf(stderr, "resolution: %s: %s\n", path, strerror(errno)); return -1; }
    fprintf(f, "# WynlandOS screen mode (resolution(1)); Zerp 2.0 reads it when it starts\n");
    if (w) fprintf(f, "mode = %ux%u\n", w, h);
    else fprintf(f, "# no mode: the boot size\n");
    if (fclose(f) != 0 || rename(tmp, path) < 0) {
        fprintf(stderr, "resolution: %s: %s\n", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

// Zerp 2.0's Hyprland IPC; false: not reachable (not in its session)
static int restart_session(void)
{
    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!sig || !rt) return 0;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s/hypr/%s/.socket.sock", rt, sig);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return 0; }
    static const char req[] = "dispatch restartsession";
    int ok = write(fd, req, sizeof req - 1) == (ssize_t)(sizeof req - 1);
    char buf[64];
    ok = ok && read(fd, buf, sizeof buf) > 0;
    close(fd);
    return ok;
}

static void show(int system)
{
    char user[512], sys[512];
    conf_path(user, sizeof user, 0);
    conf_path(sys, sizeof sys, 1);
    unsigned uw = 0, uh = 0, sw = 0, sh = 0;
    int u = system ? -1 : read_conf(user, &uw, &uh);
    int s = read_conf(sys, &sw, &sh);
    if (g_cur_w) printf("now:    %ux%u\n", g_cur_w, g_cur_h);
    if (u == 1) printf("chosen: %ux%u (%s)\n", uw, uh, user);
    else if (u == 0) printf("chosen: the boot size (%s)\n", user);
    else if (s == 1) printf("chosen: %ux%u (%s)\n", sw, sh, sys);
    else printf("chosen: the boot size\n");
    printf("modes:\n");
    for (int i = 0; i < g_nmodes; i++) {
        const struct mode_info *m = &g_modes[i];
        int now = m->hdisplay == g_cur_w && m->vdisplay == g_cur_h;
        printf("  %c %4ux%-4u%s\n", now ? '*' : ' ', m->hdisplay, m->vdisplay,
               (m->type & PREFERRED) ? "  (boot size)" : "");
    }
}

static void usage(void)
{
    fprintf(stderr,
            "usage: resolution                    the mode now and the modes there are\n"
            "       resolution WIDTHxHEIGHT       choose a mode (the desktop starts again)\n"
            "       resolution reset              back to the boot size\n"
            "options: -y  do not ask   --later  at the next login   --system  the machine's default (root)\n");
}

int main(int argc, char **argv)
{
    int yes = 0, later = 0, system = 0;
    const char *what = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-y") || !strcmp(argv[i], "--yes")) yes = 1;
        else if (!strcmp(argv[i], "--later")) later = 1;
        else if (!strcmp(argv[i], "--system")) system = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (argv[i][0] == '-' || what) { usage(); return 2; }
        else what = argv[i];
    }
    if (!read_modes()) {
        fprintf(stderr, "resolution: no display driver (/dev/dri/card0): the screen size is the firmware's,\n"
                        "chosen at boot (\\resolution.cfg on the boot partition)\n");
        return 1;
    }
    if (!what || !strcmp(what, "list")) { show(system); return 0; }

    unsigned w = 0, h = 0;
    if (strcmp(what, "reset")) {
        char x;
        if (sscanf(what, "%u%c%u", &w, &x, &h) != 3 || (x != 'x' && x != 'X') || !w || !h) { usage(); return 2; }
        if (find_mode(w, h) < 0) {
            fprintf(stderr, "resolution: %ux%u is not one of the modes (resolution: the list)\n", w, h);
            return 1;
        }
    }
    if (system && geteuid() != 0) { fprintf(stderr, "resolution: --system needs root (sudo)\n"); return 1; }

    char path[512];
    conf_path(path, sizeof path, system);
    if (write_conf(path, w, h) < 0) return 1;
    if (w) printf("chosen: %ux%u (%s)\n", w, h, path);
    else printf("chosen: the boot size (%s)\n", path);

    // the login screen's default changes nothing in this session
    if (later || system) { printf("it applies at the next login\n"); return 0; }
    if (w ? (w == g_cur_w && h == g_cur_h) : (find_mode(g_cur_w, g_cur_h) == 0)) { printf("already on screen\n"); return 0; }
    if (!getenv("HYPRLAND_INSTANCE_SIGNATURE")) { printf("not in a Zerp session: it applies at the next login\n"); return 0; }
    if (!yes) {
        if (!isatty(0)) { printf("it applies at the next login (or: resolution -y ... to restart now)\n"); return 0; }
        printf("The desktop starts again for it: open windows close. Restart now? [y/N] ");
        fflush(stdout);
        char ans[16];
        if (!fgets(ans, sizeof ans, stdin) || (ans[0] != 'y' && ans[0] != 'Y')) {
            printf("it applies at the next login\n");
            return 0;
        }
    }
    if (!restart_session()) {
        fprintf(stderr, "resolution: Zerp 2.0 did not answer; it applies at the next login\n");
        return 1;
    }
    printf("restarting the desktop...\n");
    return 0;
}
