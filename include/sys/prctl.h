#pragma once
#include <stdint.h>
#include <stddef.h>
#define PR_CAP_AMBIENT 47
#define PR_CAP_AMBIENT_LOWER 3
#define CAP_SYS_NICE 23
inline int prctl(int option, unsigned long arg2, unsigned long arg3, unsigned long arg4, unsigned long arg5) { return 0; }
