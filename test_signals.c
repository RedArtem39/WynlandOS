/*
 * WynlandOS Phase 22b regression: real POSIX-style signals.
 *
 *   T1  handler dispatch        -- sigaction(SIGUSR1)+raise(): the handler
 *                                  runs at the syscall-return boundary and
 *                                  execution resumes cleanly after it
 *   T2  any-signal dispatch     -- same mechanics for SIGSEGV raised via
 *                                  tgkill (proves delivery is independent of
 *                                  the fault path, which stays unwired v1)
 *   T3  mask defers delivery    -- BLOCK SIGUSR1 -> raise -> still pending;
 *                                  UNBLOCK -> delivered at next boundary
 *   T4  default action kills    -- child process raises SIGTERM unhandled ->
 *                                  whole process terminates (SYS_process_alive
 *                                  flips to 0); parent is unaffected
 *   T5  oldact roundtrip        -- sigaction(oldact) reports what was set
 *
 * One write(2) per line (serial-interleaving lesson).
 */

#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>

static char obuf[256];
static void out(const char *s) {
    size_t n = strlen(s);
    if (n > sizeof(obuf) - 1) n = sizeof(obuf) - 1;
    memcpy(obuf, s, n);
    obuf[n] = '\n';
    write(1, obuf, n + 1);
}

static volatile int g_hit_usr1;
static volatile int g_hit_segv;
static volatile int g_hit_deferred;

static void h_usr1(int s)   { (void)s; g_hit_usr1 = 1; }
static void h_segv(int s)   { (void)s; g_hit_segv = 1; }
static void h_def(int s)    { (void)s; g_hit_deferred = 1; }

static void nap(void) {
    struct timespec t = { 0, 20 * 1000 * 1000 };
    nanosleep(&t, 0);
}


int main(void) {
    /* T1 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h_usr1;
    sigaction(SIGUSR1, &sa, 0);
    raise(SIGUSR1); /* lands in handler at THIS syscall's return tail */
    out(g_hit_usr1 ? "T1 handler dispatch USR1: PASS"
                   : "T1 handler dispatch USR1: FAIL");

    /* T2 */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h_segv;
    sigaction(SIGSEGV, &sa, 0);
    syscall(SYS_tgkill, 1, 1, SIGSEGV);
    out(g_hit_segv ? "T2 handler dispatch SEGV via tgkill: PASS"
                   : "T2 handler dispatch SEGV via tgkill: FAIL");

    /* T3: deferral requires the signal to STAY blocked across the raise.
       libc raise() internally blocks-all/saves/restores the mask, so with a
       correct kernel the pending signal is delivered the moment raise()
       restores its own saved mask -- masking-then-libc-raising can never
       demonstrate deferral. Use a RAW tgkill instead: no mask dance, the
       signal stays genuinely pending until WE unblock. */
    g_hit_deferred = 0;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h_def;
    sigaction(SIGUSR2, &sa, 0);

    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR2);
    sigprocmask(SIG_BLOCK, &block, 0);
    syscall(SYS_tgkill, 1, 1, SIGUSR2); /* raw: no internal mask restore */
    nap();
    int deferred_ok = !g_hit_deferred; /* masked: nothing delivered yet */

    sigprocmask(SIG_UNBLOCK, &block, 0); /* delivery fires at THIS tail */
    nap(); nap();
    {
        char d[160];
        const char *d1 = deferred_ok ? "1" : "0";
        const char *d2 = g_hit_deferred ? "1" : "0";
        size_t k = 0;
        const char *pfx = "[t3] deferred_ok=";
        while (*pfx) d[k++] = *pfx++;
        d[k++] = d1[0];
        const char *mid = " hit=";
        while (*mid) d[k++] = *mid++;
        d[k++] = d2[0];
        d[k] = 0;
        write(1, "\r\n", 2);
        write(1, d, k);
        write(1, "\r\n", 2);
    }
    out((deferred_ok && g_hit_deferred)
        ? "T3 mask defers then delivers: PASS"
        : "T3 mask defers then delivers: FAIL");

    /* T4: default fatal action inside a child process */
    long cpid = fork();
    if (cpid == 0) {
        signal(SIGTERM, SIG_DFL);
        raise(SIGTERM);
        for (;;) pause(); /* never reached on a correct kernel */
    }
    int alive_after = 1;
    for (int i = 0; i < 100; i++) {
        nap();
        alive_after = (int)syscall(411 /* SYS_process_alive */, cpid);
        if (!alive_after) break;
    }
    out(!alive_after ? "T4 default SIGTERM kills child: PASS"
                     : "T4 default SIGTERM kills child: FAIL");

    /* T5: oldact roundtrip */
    struct sigaction old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h_usr1;
    sigaction(SIGUSR1, &sa, &old);
    int rt = (old.sa_handler == h_usr1);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &sa, 0);
    out(rt ? "T5 oldact roundtrip: PASS" : "T5 oldact roundtrip: FAIL");

    out("ALL SIGNAL CHECKS COMPLETE");
    return 0;
}
