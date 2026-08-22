#pragma once
#include <stdint.h>
#include <stddef.h>
#define PR_CAP_AMBIENT 47
#define PR_CAP_AMBIENT_LOWER 3
#define CAP_SYS_NICE 23
#define PR_SET_PDEATHSIG 1
#define PR_GET_PDEATHSIG 2
#define PR_SET_DUMPABLE 4
#define PR_GET_DUMPABLE 3
#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define PR_TASK_PERF_EVENTS_DISABLE 31
#define PR_TASK_PERF_EVENTS_ENABLE 32
inline int prctl(int option, ...) { return 0; }
