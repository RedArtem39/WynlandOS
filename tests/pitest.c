/*
 * WynlandOS - priority-inheriting mutexes under contention (glibc,
 * host-built): what PipeWire's loops lock (PTHREAD_PRIO_INHERIT, recursive).
 * Four threads take one such mutex 20000 times each and count; a timed
 * lock waits out a holder. One "[pitest] PASS|FAIL" line per check
 * (run by the pwtest check, /usr/lib/wynland/pwtest).
 *
 * "pitest dump": every user thread and its syscall to the serial log
 * (WynlandOS syscall 1000) -- what a stuck PipeWire client waits in.
 *
 * Build: gcc -O2 -pthread -o build/pitest.elf tests/pitest.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t m;
static long counter;
static int g_pass, g_fail;
static void check(int ok, const char *what)
{
    fprintf(stderr, "[pitest] %s %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) g_pass++; else g_fail++;
}

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 20000; i++) {
        if (pthread_mutex_lock(&m) != 0) return (void *)1;
        pthread_mutex_lock(&m);          /* recursive, as PipeWire's */
        counter++;
        pthread_mutex_unlock(&m);
        if (pthread_mutex_unlock(&m) != 0) return (void *)1;
    }
    return NULL;
}

static void *holder(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&m);
    usleep(300 * 1000);
    pthread_mutex_unlock(&m);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "dump")) { syscall(1000); return 0; }
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
    int r = pthread_mutex_init(&m, &a);
    check(r == 0, "PTHREAD_PRIO_INHERIT mutex made");
    if (r) { fprintf(stderr, "[pitest] DONE pass=%d fail=%d\n", g_pass, g_fail); return 1; }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, NULL);
    int bad = 0;
    for (int i = 0; i < 4; i++) { void *rv; pthread_join(t[i], &rv); if (rv) bad = 1; }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    fprintf(stderr, "[pitest] 4 x 20000 contended locks in %ld ms, counter %ld\n", ms, counter);
    check(!bad && counter == 80000, "contended lock/unlock counts right");

    pthread_t h;
    pthread_create(&h, NULL, holder, NULL);
    usleep(50 * 1000);
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_nsec += 50 * 1000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    check(pthread_mutex_timedlock(&m, &dl) == ETIMEDOUT, "timedlock times out on a held mutex");
    check(pthread_mutex_lock(&m) == 0, "lock waits for the holder");
    pthread_mutex_unlock(&m);
    pthread_join(h, NULL);

    fprintf(stderr, "[pitest] DONE pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
