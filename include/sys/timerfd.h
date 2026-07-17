#pragma once
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#define TFD_CLOEXEC 02000000
#define TFD_NONBLOCK 04000
#define TFD_TIMER_ABSTIME (1 << 0)

inline int timerfd_create(int clockid, int flags) { return -1; }
inline int timerfd_settime(int fd, int flags, const struct itimerspec* new_value, struct itimerspec* old_value) { return 0; }
inline int timerfd_gettime(int fd, struct itimerspec* curr_value) { return 0; }
