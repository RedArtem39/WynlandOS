/*
 * WynlandOS - the vDSO's data page (kernel/vdso.c writes it, vdso/vdso.c
 * reads it from user space): what clock_gettime() needs to tell the time
 * without entering the kernel -- the TSC clock of kernel/sched.c
 * (sched_clock_ns) and the wall clock of kernel/rtc.c, as numbers.
 *
 * Self-contained (no other headers): vdso/vdso.c is built as a user-space
 * shared object.
 */
#pragma once

#define VDSO_VVAR_ADDR  0x680000000000ULL   /* the data page; the vDSO's text right after it */
#define VDSO_MAX_CPUS   16                  /* = MAX_CPUS (include/wynland/smp.h; kernel/vdso.c checks) */

typedef struct VdsoData {
    unsigned int       ok;          /* 0: the clock is not the TSC's -- ask the kernel */
    unsigned int       use_aux;     /* rdtscp: the core's number, its TSC offset taken off */
    unsigned long long tsc0;        /* sched_clock_ns() = ms0 ms + (tsc - tsc0) / per ms */
    unsigned long long ms0;
    unsigned long long per;         /* TSC ticks per millisecond */
    unsigned long long boot_unix;   /* realtime = boot_unix s + (monotonic - base_ns) */
    unsigned long long base_ns;
    long long          off[VDSO_MAX_CPUS];   /* each core's TSC offset (kernel/smp.c) */
} VdsoData;
