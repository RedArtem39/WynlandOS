/*
 * WynlandOS - the vDSO: clock_gettime(), gettimeofday() and time() without
 * a system call, as Linux's. glibc finds it by AT_SYSINFO_EHDR and calls
 * these instead of entering the kernel -- Qt, WebKit, Chromium ask for the
 * time thousands of times a second.
 *
 * The same clock as the kernel's (kernel/sched.c: sched_clock_ns, kernel/
 * rtc.c: rtc_get_unix_time_ns), from the data page the kernel maps right
 * before this code (include/wynland/vdso.h). Anything it does not know --
 * a CPU-time clock, a timezone asked of gettimeofday, a clock that is not
 * the TSC's yet -- goes to the kernel as before.
 *
 * Built as a user-space shared object (Makefile: build/vdso.so), no libc,
 * no relocations: the data page is reached relative to the code.
 */
#include "../include/wynland/vdso.h"

extern const VdsoData vvar_page __attribute__((visibility("hidden")));

struct ts { long long sec, nsec; };
struct tv { long long sec, usec; };

static inline long sys2(long n, long a, long b)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}

static inline unsigned long long tsc_now(const volatile VdsoData *d)
{
    unsigned int lo, hi, aux;
    if (d->use_aux) {
        __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
        return (((unsigned long long)hi << 32) | lo) - (unsigned long long)d->off[aux & (VDSO_MAX_CPUS - 1)];
    }
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

/* kernel/sched.c's sched_clock_ns(), to the letter */
static inline unsigned long long mono_ns(const volatile VdsoData *d)
{
    unsigned long long per = d->per, t = tsc_now(d) - d->tsc0;
    return d->ms0 * 1000000ULL + t / per * 1000000ULL + t % per * 1000000ULL / per;
}

/* kernel/rtc.c's rtc_get_unix_time_ns() */
static inline unsigned long long real_ns(const volatile VdsoData *d)
{
    unsigned long long now = mono_ns(d);
    return d->boot_unix * 1000000000ULL + (now > d->base_ns ? now - d->base_ns : 0);
}

int __vdso_clock_gettime(int clk, struct ts *ts)
{
    const volatile VdsoData *d = &vvar_page;
    /* the kernel's split: REALTIME (0), its _COARSE (5), TAI (11) are the
       wall clock; MONOTONIC (1), _RAW (4), _COARSE (6), BOOTTIME (7) the
       uptime; the CPU-time clocks (2, 3, a thread's) only it knows */
    int real = clk == 0 || clk == 5 || clk == 11, mono = clk == 1 || clk == 4 || clk == 6 || clk == 7;
    if (!d->ok || !ts || !(real || mono)) return (int)sys2(228, clk, (long)ts);
    unsigned long long ns = real ? real_ns(d) : mono_ns(d);
    ts->sec = (long long)(ns / 1000000000ULL);
    ts->nsec = (long long)(ns % 1000000000ULL);
    return 0;
}

int __vdso_gettimeofday(struct tv *tv, void *tz)
{
    const volatile VdsoData *d = &vvar_page;
    if (!d->ok || tz) return (int)sys2(96, (long)tv, (long)tz);
    if (tv) {
        unsigned long long ns = real_ns(d);
        tv->sec = (long long)(ns / 1000000000ULL);
        tv->usec = (long long)(ns % 1000000000ULL / 1000ULL);
    }
    return 0;
}

long long __vdso_time(long long *t)
{
    const volatile VdsoData *d = &vvar_page;
    if (!d->ok) return sys2(201, (long)t, 0);
    long long s = (long long)(real_ns(d) / 1000000000ULL);
    if (t) *t = s;
    return s;
}

/* the names glibc asks for (versioned LINUX_2.6: vdso/vdso.lds) */
int clock_gettime(int, struct ts *) __attribute__((weak, alias("__vdso_clock_gettime")));
int gettimeofday(struct tv *, void *) __attribute__((weak, alias("__vdso_gettimeofday")));
long long time(long long *) __attribute__((weak, alias("__vdso_time")));
