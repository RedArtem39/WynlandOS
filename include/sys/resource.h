#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/time.h>

typedef unsigned long rlim_t;
struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

#define RLIMIT_NOFILE 7

struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
    long ru_ixrss;
    long ru_idrss;
    long ru_isrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nswap;
    long ru_inblock;
    long ru_oublock;
    long ru_msgsnd;
    long ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw;
    long ru_nivcsw;
};

#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)

inline int getrlimit(int resource, struct rlimit *rlim) { return 0; }
inline int setrlimit(int resource, const struct rlimit *rlim) { return 0; }
inline int getrusage(int who, struct rusage *usage) { return -1; }
