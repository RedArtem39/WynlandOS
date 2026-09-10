/*
 * WynlandOS Phase 22d regression: real blocking pipe I/O + real poll() wake.
 *
 *   T1  pipe read genuinely blocks -- a fork()'d child sleeps ~300ms then
 *       writes; the parent's read() call must take roughly that long and
 *       return real data, not the old "count==0 -> return 0 instantly"
 *       behavior (which would make this test's own timing measurement
 *       come back near-zero with 0 bytes).
 *   T2  pipe write genuinely blocks when full -- parent fills the pipe
 *       completely, forks a child that sleeps ~300ms then drains a few
 *       bytes, then the parent's next write() must take roughly that
 *       long to complete instead of silently truncating to 0 bytes written.
 *   T3  poll() on a single pipe fd wakes PROMPTLY when data arrives,
 *       not just at the full timeout -- proves the real per-fd wait
 *       queue path (kernel/syscall.c's single_wq fast path), not merely
 *       "sleeps in small increments until the deadline".
 *
 * One write(2) per line (serial-interleaving lesson, same as test_futex.c).
 * Build:
 *   x86_64-linux-musl-gcc -static -O2 -o test_blockio.elf test_blockio.c
 */
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <sys/wait.h>

static char obuf[256];
static void out(const char *s) {
    size_t n = strlen(s);
    if (n > sizeof(obuf) - 1) n = sizeof(obuf) - 1;
    memcpy(obuf, s, n);
    obuf[n] = '\n';
    write(1, obuf, n + 1);
}

static void outf_ms(const char *label, long ms) {
    char buf[64];
    int len = 0;
    const char *p = label;
    while (*p) buf[len++] = *p++;
    buf[len++] = '=';
    char digits[16];
    int dn = 0;
    long v = ms;
    if (v == 0) digits[dn++] = '0';
    while (v > 0) { digits[dn++] = (char)('0' + (v % 10)); v /= 10; }
    while (dn > 0) buf[len++] = digits[--dn];
    buf[len++] = '\n';
    write(1, buf, len);
}

static long elapsed_ms(struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000L + (now.tv_nsec - start->tv_nsec) / 1000000L;
}

static void nap_ms(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, 0);
}

int main(void) {
    /* T1: blocking read wakes on a delayed write */
    {
        int pfd[2];
        int ok = (pipe(pfd) == 0);
        if (ok) {
            long pid = fork();
            if (pid == 0) {
                close(pfd[0]);
                nap_ms(300);
                write(pfd[1], "HELLO", 5);
                _exit(0);
            }
            close(pfd[1]);
            struct timespec start;
            clock_gettime(CLOCK_MONOTONIC, &start);
            char buf[8] = {0};
            int n = (int)read(pfd[0], buf, sizeof(buf));
            long ms = elapsed_ms(&start);
            outf_ms("[t1] elapsed_ms", ms);
            int waited_for_real = (ms >= 150); /* comfortably below the ~300ms delay, comfortably above "returned instantly" */
            int got_data = (n == 5 && memcmp(buf, "HELLO", 5) == 0);
            out((waited_for_real && got_data) ? "T1 pipe read blocks and wakes: PASS" : "T1 pipe read blocks and wakes: FAIL");
            close(pfd[0]);
            int status;
            waitpid(pid, &status, 0);
        } else {
            out("T1 pipe read blocks and wakes: FAIL (pipe() failed)");
        }
    }

    /* T2: blocking write wakes once the child drains space */
    {
        int pfd[2];
        int ok = (pipe(pfd) == 0);
        char filler[4096];
        memset(filler, 'X', sizeof(filler));
        if (ok) {
            /* Fill the pipe completely -- PIPE_BUF_SIZE is 4096 (kernel/syscall.c). */
            int total = 0;
            while (total < 4096) {
                int n = (int)write(pfd[1], filler + total, 4096 - total);
                if (n <= 0) break;
                total += n;
            }
            ok = (total == 4096);
        }
        if (ok) {
            long pid = fork();
            if (pid == 0) {
                close(pfd[1]);
                nap_ms(300);
                char drain[64];
                read(pfd[0], drain, sizeof(drain)); /* frees space for the parent's blocked write */
                _exit(0);
            }
            close(pfd[0]);
            struct timespec start;
            clock_gettime(CLOCK_MONOTONIC, &start);
            int n = (int)write(pfd[1], "MORE", 4);
            long ms = elapsed_ms(&start);
            outf_ms("[t2] elapsed_ms", ms);
            int waited_for_real = (ms >= 150);
            int wrote_something = (n > 0);
            out((waited_for_real && wrote_something) ? "T2 pipe write blocks and wakes: PASS" : "T2 pipe write blocks and wakes: FAIL");
            close(pfd[1]);
            int status;
            waitpid(pid, &status, 0);
        } else {
            out("T2 pipe write blocks and wakes: FAIL (setup failed)");
        }
    }

    /* T3: poll() wakes promptly, not just at the full timeout */
    {
        int pfd[2];
        int ok = (pipe(pfd) == 0);
        if (ok) {
            long pid = fork();
            if (pid == 0) {
                close(pfd[0]);
                nap_ms(200);
                write(pfd[1], "X", 1);
                _exit(0);
            }
            close(pfd[1]);
            struct pollfd pfd_watch;
            pfd_watch.fd = pfd[0];
            pfd_watch.events = POLLIN;
            pfd_watch.revents = 0;

            struct timespec start;
            clock_gettime(CLOCK_MONOTONIC, &start);
            int r = poll(&pfd_watch, 1, 5000); /* 5s timeout -- should NOT need anywhere near this */
            long ms = elapsed_ms(&start);
            outf_ms("[t3] elapsed_ms", ms);
            int woke_promptly = (ms < 2000); /* well under the 5000ms timeout budget */
            int reported_ready = (r == 1 && (pfd_watch.revents & POLLIN));
            out((woke_promptly && reported_ready) ? "T3 poll wakes promptly: PASS" : "T3 poll wakes promptly: FAIL");
            close(pfd[0]);
            int status;
            waitpid(pid, &status, 0);
        } else {
            out("T3 poll wakes promptly: FAIL (pipe() failed)");
        }
    }

    out("ALL BLOCKIO CHECKS COMPLETE");
    return 0;
}
