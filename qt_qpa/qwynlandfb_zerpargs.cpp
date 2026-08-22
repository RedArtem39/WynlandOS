#include "qwynlandfb_zerpargs.h"
#include <cstdlib>

namespace {
int g_c2s_fd = -1;
int g_s2c_fd = -1;
int g_shm_fd = -1;
}

extern "C" void zerp_qpa_capture_args(int argc, char **argv)
{
    if (argc < 4) return;
    g_c2s_fd = atoi(argv[1]);
    g_s2c_fd = atoi(argv[2]);
    g_shm_fd = atoi(argv[3]);
}

extern "C" int zerp_qpa_c2s_fd() { return g_c2s_fd; }
extern "C" int zerp_qpa_s2c_fd() { return g_s2c_fd; }
extern "C" int zerp_qpa_shm_fd() { return g_shm_fd; }
