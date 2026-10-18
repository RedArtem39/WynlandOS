#include "qwynlandfb_zerpargs.h"
#include <cstdlib>

namespace {
int g_c2s_fd = -1;
int g_s2c_fd = -1;
int g_shm_fd = -1;

bool all_digits(const char *s)
{
    if (!s || !*s) return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return false;
    return true;
}
}

// Zerp's three fd numbers come first, as plain numbers; anything else (a
// program started some other way: `term -e nano`) has none
extern "C" int zerp_qpa_capture_args(int argc, char **argv)
{
    if (argc < 4 || !all_digits(argv[1]) || !all_digits(argv[2]) || !all_digits(argv[3])) return 0;
    g_c2s_fd = atoi(argv[1]);
    g_s2c_fd = atoi(argv[2]);
    g_shm_fd = atoi(argv[3]);
    return 3;
}

extern "C" const char *zerp_qpa_platform()
{
    if (g_c2s_fd >= 0) return "qwynlandfb";
    const char *wl = getenv("WAYLAND_DISPLAY");
    return wl && *wl ? "wayland" : "qwynlandfb";
}

extern "C" int zerp_qpa_c2s_fd() { return g_c2s_fd; }
extern "C" int zerp_qpa_s2c_fd() { return g_s2c_fd; }
extern "C" int zerp_qpa_shm_fd() { return g_shm_fd; }
