#pragma once
#include <stdint.h>
#include <stddef.h>

typedef unsigned long rlim_t;
struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

#define RLIMIT_NOFILE 7
inline int getrlimit(int resource, struct rlimit *rlim) { return 0; }
inline int setrlimit(int resource, const struct rlimit *rlim) { return 0; }
