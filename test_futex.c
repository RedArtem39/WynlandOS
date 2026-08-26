/*
 * WynlandOS Phase 22a regression: REAL futex sleep/wake semantics.
 *
 * Exercises the new kernel wait queues through genuine musl pthreads
 * primitives (mutexes/condvars compile down to FUTEX_WAIT/WAKE with the
 * PRIVATE flag; timed waits use FUTEX_WAIT_BITSET):
 *
 *   1. wake-after-delay   -- main really sleeps inside the kernel until a
 *                            helper wakes the word (~150ms measured, proving
 *                            blocking instead of yield-spinning)
 *   2. value-changed race -- FUTEX_WAIT on a mismatched value returns
 *                            -EAGAIN immediately (the canonical contract)
 *   3. timed waits        -- already-expired absolute timeout returns
 *                            -ETIMEDOUT instantly; a +250ms one actually
 *                            waits ~250ms then times out
 *   4. mutex stress       -- 4 detached threads x 500 increments under
 *                            pthread_mutex = exactly 2000 (no lost wakes)
 *   5. condvar pipeline   -- producer/consumer hand off 32 items through a
 *                            condvar, each item accounted exactly once
 *
 * pthread_join() is deliberately NOT used anywhere: it needs
 * CLONE_CHILD_CLEARTID semantics (Phase 22e), which this kernel doesn't
 * implement yet -- threads here are fire-and-forget, completion tracked
 * through plain atomic counters.
 *
 * Output uses ONE write(2,...) per line built into a single buffer -- the
 * project's known serial-interleaving lesson (Phase 11 methodology note).
 */

#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>
#include <stdint.h>

#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_WAIT_BITSET   9
#define FUTEX_PRIVATE_FLAG  128

static long sys_futex(volatile uint32_t *uaddr, int op, uint32_t val,
                      const struct timespec *timeout, uint32_t *uaddr2,
                      uint32_t val3) {
    return syscall(SYS_futex, uaddr, op, val, timeout, uaddr2, val3);
}

static char obuf[256];
static void out(const char *s) {
    size_t n = strlen(s);
    if (n > sizeof(obuf) - 1) n = sizeof(obuf) - 1;
    memcpy(obuf, s, n);
    obuf[n] = '\n';
    write(1, obuf, n + 1); /* one atomic write per line */
}

static void now_mono(struct timespec *ts) { clock_gettime(CLOCK_MONOTONIC, ts); }
static long long elms(const struct timespec *a, const struct timespec *b) {
    return (b->tv_sec - a->tv_sec) * 1000LL + (b->tv_nsec - a->tv_nsec) / 1000000LL;
}
static void nap_us(unsigned us) {
    struct timespec t = { 0, (long)us * 1000 };
    nanosleep(&t, 0);
}

/* ---- 1. wake after delay ---- */
static uint32_t wake_word;
static void *waker_thread(void *arg) {
    (void)arg;
    nap_us(200000); /* let main get inside its futex wait first */
    __atomic_fetch_add(&wake_word, 1, __ATOMIC_SEQ_CST);
    sys_futex(&wake_word, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0);
    return 0;
}

/* ---- 4. mutex stress ---- */
#define STRESS_THREADS 4
#define STRESS_ITERS   500
static pthread_mutex_t stress_mu = PTHREAD_MUTEX_INITIALIZER;
static long stress_counter;
static long stress_done;
static void *stress_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < STRESS_ITERS; i++) {
        pthread_mutex_lock(&stress_mu);
        stress_counter++;
        pthread_mutex_unlock(&stress_mu);
        if ((i & 63) == 0) nap_us(200); /* provoke preemption mid-run */
    }
    __atomic_fetch_add(&stress_done, 1, __ATOMIC_SEQ_CST);
    return 0;
}

/* ---- 5. condvar pipeline ---- */
#define PIPE_ITEMS 32
static pthread_mutex_t pipe_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  pipe_cv = PTHREAD_COND_INITIALIZER;
static int pipe_buf[PIPE_ITEMS];
static int pipe_count;
static int consumed[PIPE_ITEMS];
static volatile int pipe_shutdown;
static void *consumer_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&pipe_mu);
        while (pipe_count == 0 && !pipe_shutdown)
            pthread_cond_wait(&pipe_cv, &pipe_mu);
        if (pipe_count == 0 && pipe_shutdown) { pthread_mutex_unlock(&pipe_mu); return 0; }
        int v = pipe_buf[--pipe_count];
        pthread_mutex_unlock(&pipe_mu);
        consumed[v]++;
    }
}

int main(void) {
    struct timespec a, b;
    static pthread_t th;

    /* 1. real blocking until woken (armed delay avoids the wake-before-wait
       race: helper sleeps 200ms, main arms+waits immediately) */
    wake_word = 0;
    pthread_create(&th, 0, waker_thread, 0);
    now_mono(&a);
    long r = sys_futex(&wake_word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, 0, 0, 0);
    now_mono(&b);
    long long ms = elms(&a, &b);
    out((r == 0 && wake_word == 1 && ms >= 100)
        ? "T1 wake-after-delay: PASS"
        : "T1 wake-after-delay: FAIL");

    /* 2. canonical EAGAIN race path */
    uint32_t v = 123;
    r = sys_futex(&v, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 999 /* wrong */, 0, 0, 0);
    out((r == -1 && errno == EAGAIN)
        ? "T2 value-changed EAGAIN: PASS"
        : "T2 value-changed EAGAIN: FAIL");

    /* 3a. already-expired absolute timeout -> immediate ETIMEDOUT */
    struct timespec past;
    clock_gettime(CLOCK_MONOTONIC, &past);
    past.tv_sec -= 10;
    now_mono(&a);
    r = sys_futex(&v, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, 123, &past, 0, ~0u);
    now_mono(&b);
    out((r == -1 && errno == ETIMEDOUT && elms(&a, &b) < 50)
        ? "T3a expired-timeout instant ETIMEDOUT: PASS"
        : "T3a expired-timeout instant ETIMEDOUT: FAIL");

    /* 3b. future timeout actually sleeps ~250ms then ETIMEDOUTs */
    clock_gettime(CLOCK_MONOTONIC, &past);
    past.tv_sec += 0;
    past.tv_nsec += 250 * 1000000L;
    if (past.tv_nsec >= 1000000000L) { past.tv_nsec -= 1000000000L; past.tv_sec++; }
    now_mono(&a);
    r = sys_futex(&v, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, 123, &past, 0, ~0u);
    now_mono(&b);
    ms = elms(&a, &b);
    out((r == -1 && errno == ETIMEDOUT && ms >= 180 && ms < 3000)
        ? "T3b timed-wait ~250ms: PASS"
        : "T3b timed-wait ~250ms: FAIL");

    /* 4. contended mutex stress: exact totals prove no lost wakes */
    stress_counter = 0; stress_done = 0;
    for (int i = 0; i < STRESS_THREADS; i++)
        pthread_create(&th, 0, stress_thread, 0);
    while (__atomic_load_n(&stress_done, __ATOMIC_SEQ_CST) < STRESS_THREADS)
        nap_us(1000);
    out((stress_counter == (long)STRESS_THREADS * STRESS_ITERS)
        ? "T4 mutex stress 4x500: PASS (exact counter)"
        : "T4 mutex stress: FAIL (lost updates)");

    /* 5. condvar producer/consumer */
    pipe_count = 0; pipe_shutdown = 0;
    memset(consumed, 0, sizeof(consumed));
    pthread_create(&th, 0, consumer_thread, 0);
    int ok = 1;
    for (int i = 0; i < PIPE_ITEMS; i++) {
        pthread_mutex_lock(&pipe_mu);
        pipe_buf[pipe_count++] = i;
        pthread_cond_signal(&pipe_cv);
        pthread_mutex_unlock(&pipe_mu);
        nap_us(1000);
    }
    pthread_mutex_lock(&pipe_mu);
    pipe_shutdown = 1;
    pthread_cond_broadcast(&pipe_cv);
    pthread_mutex_unlock(&pipe_mu);
    for (int spin = 0; spin < 400; spin++) { nap_us(1000); if (consumed[PIPE_ITEMS-1]) break; }
    for (int i = 0; i < PIPE_ITEMS; i++) if (consumed[i] != 1) ok = 0;
    out(ok ? "T5 condvar pipeline 32 items: PASS"
           : "T5 condvar pipeline: FAIL");

    out("ALL FUTEX CHECKS COMPLETE");
    return 0;
}
