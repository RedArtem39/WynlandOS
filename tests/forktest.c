/*
 * WynlandOS - process lifecycle test (glibc, host-built)
 * ============================================================
 * fork/vfork/posix_spawn, wait4/waitpid/waitid, exit statuses, fatal
 * signals, kill(), exit_group from a helper thread, process groups and
 * sessions, SIGCHLD. One "[forktest] PASS|FAIL ..." line per check.
 *
 * Build: gcc -O2 -pthread -o build/forktest.elf tests/forktest.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <signal.h>
#include <spawn.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

extern char **environ;
static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    fprintf(stderr, "[forktest] %s %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) g_pass++; else g_fail++;
}

static volatile sig_atomic_t g_sigchld;
static void on_sigchld(int sig) { (void)sig; g_sigchld++; }

static void *exit_from_thread(void *arg)
{
    (void)arg;
    exit(3); /* exit_group: must end the whole process, main is asleep */
    return NULL;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* fork + execve of a static (musl) port with stdout on a pipe, the way the
   Zerp terminal runs /usr/bin programs. Returns the exit status, output in buf. */
static int run_capture(const char *path, const char *arg, const char *arg2, char *buf, int cap)
{
    int fd[2];
    if (pipe(fd) != 0) return -100;
    pid_t p = fork();
    if (p == 0) {
        dup2(fd[1], 1); dup2(fd[1], 2);
        close(fd[0]); close(fd[1]);
        char *av[] = { (char *)path, (char *)arg, (char *)arg2, NULL };
        char *ev[] = { "TERM=vt100", "CURL_CA_BUNDLE=/etc/ssl/cert.pem", NULL };
        execve(path, av, ev);
        _exit(127);
    }
    close(fd[1]);
    int n = 0, r;
    while (n < cap - 1 && (r = (int)read(fd[0], buf + n, cap - 1 - n)) > 0) n += r;
    buf[n] = 0;
    close(fd[0]);
    int st = 0;
    if (waitpid(p, &st, 0) != p) return -101;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st);
}

/* many futex waiters at once, each on its own word */
#define FUTEX_THREADS 120
static int g_fwords[FUTEX_THREADS];
static long g_fres[FUTEX_THREADS];
static void *futex_waiter(void *arg)
{
    long i = (long)arg;
    struct timespec ts = { 5, 0 };
    long r = syscall(SYS_futex, &g_fwords[i], 128 /* FUTEX_WAIT_PRIVATE */, 0, &ts, NULL, 0);
    g_fres[i] = r < 0 ? -errno : r;
    return NULL;
}

/* EEVDF: busy threads, twice as many as cores, half of them at nice 10:
   the nice-0 ones must get several times the work done (weights 1024 vs
   110: ~9x) */
static volatile int g_spin_stop;
struct spinner { int nice; volatile unsigned long count; };
static void *spinner(void *arg)
{
    struct spinner *s = arg;
    setpriority(PRIO_PROCESS, 0, s->nice);   /* this thread (who 0) */
    while (!g_spin_stop) s->count++;
    return NULL;
}

/* syscalls outside the big kernel lock (futex, clocks, sleeping): many
   threads on several cores at once must still add up */
#define HAMMER_THREADS 8
#define HAMMER_ROUNDS  20000
static pthread_mutex_t g_hammer_mx = PTHREAD_MUTEX_INITIALIZER;
static volatile long g_hammer_count;
static void *hammer(void *arg)
{
    (void)arg;
    for (int i = 0; i < HAMMER_ROUNDS; i++) {
        pthread_mutex_lock(&g_hammer_mx);
        long c = g_hammer_count;
        if ((i & 63) == 0) sched_yield();   /* contention: holders get preempted */
        g_hammer_count = c + 1;
        pthread_mutex_unlock(&g_hammer_mx);
    }
    return NULL;
}

/* two threads hand a token back and forth through a condition variable */
static pthread_mutex_t g_pp_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pp_cv = PTHREAD_COND_INITIALIZER;
static int g_pp_turn, g_pp_left;
static void *pingpong(void *arg)
{
    int me = (int)(long)arg;
    pthread_mutex_lock(&g_pp_mx);
    while (g_pp_left > 0) {
        while (g_pp_turn != me && g_pp_left > 0) pthread_cond_wait(&g_pp_cv, &g_pp_mx);
        if (g_pp_left <= 0) break;
        g_pp_left--;
        g_pp_turn = !me;
        pthread_cond_broadcast(&g_pp_cv);
    }
    pthread_mutex_unlock(&g_pp_mx);
    return NULL;
}

/* a long sleep cut short by a signal from another thread */
static void on_usr1(int sig) { (void)sig; }
static volatile int g_sleep_ready;
static void *long_sleeper(void *out)
{
    struct timespec ts = { 5, 0 };
    long t0 = now_ms();
    /* no syscall between this and the sleep: a signal sent now stays
       pending (delivered on the way out of the kernel) and nanosleep sees it */
    g_sleep_ready = 1;
    int r = nanosleep(&ts, NULL);
    int e = errno;
    long dt = now_ms() - t0;
    if (!(r < 0 && e == EINTR)) fprintf(stderr, "[forktest] nanosleep: %d errno %d after %ld ms\n", r, e, dt);
    *(long *)out = (r < 0 && e == EINTR) ? dt : -1;
    return NULL;
}

/* pipes and eventfds outside the big lock: writers on several cores */
#define PW_THREADS 4
#define PW_RECORDS 2000
#define PW_LEN 100
static int g_pw_fd;
static void *pipe_writer(void *arg)
{
    unsigned char rec[PW_LEN];
    for (int i = 0; i < PW_RECORDS; i++) {
        memset(rec, (int)(long)arg, sizeof rec);   /* a record is one byte value throughout */
        if (write(g_pw_fd, rec, sizeof rec) != (ssize_t)sizeof rec) return (void *)1L;
    }
    return NULL;
}
static int g_ev_fd;
static void *ev_adder(void *arg)
{
    (void)arg;
    for (int i = 0; i < 5000; i++) {
        uint64_t one = 1;
        if (write(g_ev_fd, &one, 8) != 8) return (void *)1L;
    }
    return NULL;
}
static int g_pp_wfd;
static void *late_closer(void *arg)
{
    (void)arg;
    struct timespec n = { 0, 100000000 };
    nanosleep(&n, NULL);
    close(g_pp_wfd);
    return NULL;
}
static int g_pollw_fd;
static void *poll_feeder(void *arg)
{
    (void)arg;
    for (int i = 0; i < 2000; i++) {
        char c = 'x';
        if (write(g_pollw_fd, &c, 1) != 1) return (void *)1L;
        if ((i & 7) == 0) sched_yield();
    }
    return NULL;
}

/* Unix sockets outside the big lock */
static int g_sp_fd;
static void *seq_sender(void *arg)
{
    unsigned char rec[PW_LEN];
    memset(rec, (int)(long)arg, sizeof rec);
    for (int i = 0; i < PW_RECORDS; i++)
        if (send(g_sp_fd, rec, sizeof rec, 0) != (ssize_t)sizeof rec) return (void *)1L;
    return NULL;
}
static int g_fdpass_sock;
static void *fd_passer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 200; i++) {
        int efd = eventfd((unsigned)i + 1, 0);
        char b = 'f';
        struct iovec io = { &b, 1 };
        union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u;
        struct msghdr mh = { 0 };
        mh.msg_iov = &io; mh.msg_iovlen = 1;
        mh.msg_control = u.buf; mh.msg_controllen = sizeof u.buf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &efd, sizeof(int));
        ssize_t r = sendmsg(g_fdpass_sock, &mh, 0);
        close(efd);
        if (r != 1) return (void *)1L;
    }
    return NULL;
}
static int g_us_close_fd;
static void *us_late_closer(void *arg)
{
    (void)arg;
    struct timespec n = { 0, 100000000 };
    nanosleep(&n, NULL);
    close(g_us_close_fd);
    return NULL;
}

static void *report_tid(void *out)
{
    *(pid_t *)out = (pid_t)syscall(SYS_gettid);
    return NULL;
}

int main(int argc, char **argv)
{
    {   /* Linux thread ids: the main thread's is the pid (WebKit finds its
           main thread that way), another thread's is neither */
        pid_t other = 0;
        pthread_t th;
        pthread_create(&th, NULL, report_tid, &other);
        pthread_join(th, NULL);
        check((pid_t)syscall(SYS_gettid) == getpid(), "main thread: gettid() == getpid()");
        check(other > 0 && other != getpid(), "second thread: its own tid");
    }
    {   /* ftruncate and record locks, as SQLite (WebKit's storage) uses them */
        const char *p = "/tmp/forktest-trunc";
        int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
        static char buf[10000];
        memset(buf, 'x', sizeof buf);
        int ok = fd >= 0 && write(fd, buf, sizeof buf) == (ssize_t)sizeof buf;
        struct stat st;
        check(ok && ftruncate(fd, 3000) == 0 && fstat(fd, &st) == 0 && st.st_size == 3000, "ftruncate shrinks a file");
        char back[5000];
        int zeros = ftruncate(fd, 8000) == 0 && pread(fd, back, 5000, 3000) == 5000;
        for (int i = 0; zeros && i < 5000; i++) if (back[i]) zeros = 0;
        check(zeros, "ftruncate grows it with zeros (not the old bytes)");
        struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1 };
        check(fd >= 0 && fcntl(fd, F_GETLK, &fl) == 0 && fl.l_type == F_UNLCK, "F_GETLK: no conflicting lock");
        if (fd >= 0) close(fd);
        unlink(p);
    }
    {   /* eventfd is a counter: GLib wakes its loop with one write each and
           drains it once -- as a pipe of records it filled up and the
           writer blocked on itself */
        int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        int all = efd >= 0;
        uint64_t one = 1, got = 0;
        for (int i = 0; all && i < 5000; i++)
            if (write(efd, &one, 8) != 8) all = 0;
        check(all, "eventfd: 5000 writes without blocking");
        check(read(efd, &got, 8) == 8 && got == 5000, "eventfd: one read takes the sum");
        check(read(efd, &got, 8) < 0 && errno == EAGAIN, "eventfd: then empty (EAGAIN)");
        if (efd >= 0) close(efd);
    }
    {   /* 120 threads asleep on 120 futex words: the kernel had 64 queues
           for the whole system and failed the rest with ENOMEM */
        pthread_t th[FUTEX_THREADS];
        int started = 0;
        for (long i = 0; i < FUTEX_THREADS; i++)
            if (pthread_create(&th[i], NULL, futex_waiter, (void *)i) == 0) started++;
        struct timespec nap = { 0, 300000000 };
        nanosleep(&nap, NULL);                      /* let them all park */
        for (int i = 0; i < FUTEX_THREADS; i++) {
            g_fwords[i] = 1;
            syscall(SYS_futex, &g_fwords[i], 129 /* FUTEX_WAKE_PRIVATE */, 1, NULL, NULL, 0);
        }
        int bad = 0;
        for (int i = 0; i < started; i++) {
            pthread_join(th[i], NULL);
            /* 0 = woken, -EAGAIN = the word changed before it slept: both fine */
            if (g_fres[i] != 0 && g_fres[i] != -EAGAIN) bad++;
        }
        if (bad) fprintf(stderr, "[forktest] %d futex waits failed (first: %ld)\n", bad, g_fres[0]);
        check(started == FUTEX_THREADS && bad == 0, "120 futex waiters at once");
    }
    {   /* a mutex hammered by 8 threads (futex outside the big lock) */
        pthread_t th[HAMMER_THREADS];
        long t0 = now_ms();
        for (long i = 0; i < HAMMER_THREADS; i++) pthread_create(&th[i], NULL, hammer, NULL);
        for (int i = 0; i < HAMMER_THREADS; i++) pthread_join(th[i], NULL);
        fprintf(stderr, "[forktest] mutex: %ld increments in %ld ms\n", g_hammer_count, now_ms() - t0);
        check(g_hammer_count == (long)HAMMER_THREADS * HAMMER_ROUNDS, "mutex: 8 threads, no increment lost");
    }
    {   /* condvar ping-pong: every wake reaches its waiter */
        pthread_t a, b;
        g_pp_left = 4000;
        long t0 = now_ms();
        pthread_create(&a, NULL, pingpong, (void *)0L);
        pthread_create(&b, NULL, pingpong, (void *)1L);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
        long dt = now_ms() - t0;
        fprintf(stderr, "[forktest] condvar: 4000 hand-offs in %ld ms\n", dt);
        check(g_pp_left == 0 && dt < 20000, "condvar: 4000 hand-offs, none lost");
    }
    {   /* bad pointers into syscalls outside the big lock: -EFAULT, not a
           kernel crash -- unmapped, the kernel half, a read-only page */
        long r1 = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, (void *)8);
        int e1 = errno;
        long r2 = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, (void *)0xFFFF800000001000UL);
        int e2 = errno;
        void *ro = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        long r3 = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, ro);
        int e3 = errno;
        long r4 = syscall(SYS_futex, (int *)16, 128 /* FUTEX_WAIT_PRIVATE */, 0, NULL, NULL, 0);
        int e4 = errno;
        struct timespec *bad = (struct timespec *)(uintptr_t)24;
        long r5 = syscall(SYS_nanosleep, bad, NULL);
        int e5 = errno;
        check(r1 == -1 && e1 == EFAULT && r2 == -1 && e2 == EFAULT && r3 == -1 && e3 == EFAULT,
              "clock_gettime: bad, kernel and read-only pointers are EFAULT");
        check(r4 == -1 && e4 == EFAULT && r5 == -1 && e5 == EFAULT, "futex/nanosleep: bad pointers are EFAULT");
        /* a fresh lazy page: the copy faults it in */
        struct timespec *lazy = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check(lazy != MAP_FAILED && syscall(SYS_clock_gettime, CLOCK_MONOTONIC, lazy) == 0 && lazy->tv_sec >= 0,
              "clock_gettime into an untouched page");
        if (ro != MAP_FAILED) munmap(ro, 4096);
        if (lazy != MAP_FAILED) munmap(lazy, 4096);
    }
    {   /* 4 threads write 100-byte records into one pipe: every record
           arrives whole (PIPE_BUF), none is lost */
        int fd[2];
        int ok = pipe(fd) == 0;
        pthread_t th[PW_THREADS];
        g_pw_fd = fd[1];
        for (long i = 0; ok && i < PW_THREADS; i++) pthread_create(&th[i], NULL, pipe_writer, (void *)(i + 1));
        long got = 0, torn = 0, per[PW_THREADS + 1] = {0};
        unsigned char rec[PW_LEN];
        size_t have = 0;
        while (ok && got < (long)PW_THREADS * PW_RECORDS) {
            ssize_t r = read(fd[0], rec + have, sizeof rec - have);
            if (r <= 0) break;
            have += (size_t)r;
            if (have < sizeof rec) continue;
            have = 0;
            int v = rec[0];
            for (int k = 1; k < PW_LEN; k++) if (rec[k] != v) { torn++; break; }
            if (v >= 1 && v <= PW_THREADS) per[v]++;
            got++;
        }
        long bad = 0;
        for (int i = 0; ok && i < PW_THREADS; i++) { void *rv; pthread_join(th[i], &rv); if (rv) bad++; }
        for (int i = 1; i <= PW_THREADS; i++) if (per[i] != PW_RECORDS) bad++;
        if (ok) { close(fd[0]); close(fd[1]); }
        fprintf(stderr, "[forktest] pipe: %ld records, %ld torn, %ld bad\n", got, torn, bad);
        check(ok && got == (long)PW_THREADS * PW_RECORDS && torn == 0 && bad == 0,
              "pipe: 4 writer threads, every 100-byte record whole");
    }
    {   /* eventfd: 4 threads add 5000 each */
        g_ev_fd = eventfd(0, 0);
        pthread_t th[4];
        for (long i = 0; i < 4; i++) pthread_create(&th[i], NULL, ev_adder, NULL);
        for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
        uint64_t sum = 0;
        check(g_ev_fd >= 0 && read(g_ev_fd, &sum, 8) == 8 && sum == 20000, "eventfd: 4 threads, the sum adds up");
        if (g_ev_fd >= 0) close(g_ev_fd);
    }
    {   /* a reader asleep on an empty pipe gets EOF when another thread
           closes the write end */
        int fd[2];
        pthread_t th;
        int ok = pipe(fd) == 0;
        g_pp_wfd = fd[1];
        if (ok) pthread_create(&th, NULL, late_closer, NULL);
        char c;
        long t0 = now_ms();
        ssize_t r = ok ? read(fd[0], &c, 1) : -1;
        long dt = now_ms() - t0;
        if (ok) { pthread_join(th, NULL); close(fd[0]); }
        check(r == 0 && dt < 1500, "pipe: closing the write end wakes a sleeping reader (EOF)");
    }
    {   /* poll() on a pipe, fed one byte at a time from another thread:
           every wake-up arrives (no poll sleeps out its timeout) */
        int fd[2];
        pthread_t th;
        int ok = pipe(fd) == 0;
        g_pollw_fd = fd[1];
        if (ok) pthread_create(&th, NULL, poll_feeder, NULL);
        long got = 0, stalls = 0;
        while (ok && got < 2000) {
            struct pollfd pf = { fd[0], POLLIN, 0 };
            long t0 = now_ms();
            int n = poll(&pf, 1, 3000);
            if (now_ms() - t0 > 1000) stalls++;
            if (n <= 0) break;
            char buf[64];
            ssize_t r = read(fd[0], buf, sizeof buf);
            if (r <= 0) break;
            got += r;
        }
        if (ok) { pthread_join(th, NULL); close(fd[0]); close(fd[1]); }
        check(got == 2000 && stalls == 0, "poll: a pipe written by another thread, no wake-up lost");
    }
    {   /* a seqpacket socket fed by 4 threads: every message whole */
        int sv[2];
        int ok = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0;
        pthread_t th[PW_THREADS];
        g_sp_fd = ok ? sv[0] : -1;
        for (long i = 0; ok && i < PW_THREADS; i++) pthread_create(&th[i], NULL, seq_sender, (void *)(i + 1));
        long got = 0, torn = 0, per[PW_THREADS + 1] = {0};
        while (ok && got < (long)PW_THREADS * PW_RECORDS) {
            unsigned char rec[PW_LEN * 2];
            ssize_t r = recv(sv[1], rec, sizeof rec, 0);
            if (r <= 0) break;
            if (r != PW_LEN) { torn++; continue; }
            int v = rec[0];
            for (int k = 1; k < PW_LEN; k++) if (rec[k] != v) { torn++; break; }
            if (v >= 1 && v <= PW_THREADS) per[v]++;
            got++;
        }
        long bad = 0;
        for (int i = 0; ok && i < PW_THREADS; i++) { void *rv; pthread_join(th[i], &rv); if (rv) bad++; }
        for (int i = 1; i <= PW_THREADS; i++) if (per[i] != PW_RECORDS) bad++;
        if (ok) { close(sv[0]); close(sv[1]); }
        fprintf(stderr, "[forktest] seqpacket: %ld messages, %ld torn, %ld bad\n", got, torn, bad);
        check(ok && got == (long)PW_THREADS * PW_RECORDS && torn == 0 && bad == 0,
              "unix seqpacket: 4 sender threads, every message whole");
    }
    {   /* SCM_RIGHTS from another thread, 200 times: each fd arrives and works */
        int sv[2];
        int ok = socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
        pthread_t th;
        g_fdpass_sock = ok ? sv[0] : -1;
        if (ok) pthread_create(&th, NULL, fd_passer, NULL);
        int good = 0;
        for (int i = 0; ok && i < 200; i++) {
            char b;
            struct iovec io = { &b, 1 };
            union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u;
            struct msghdr mh = { 0 };
            mh.msg_iov = &io; mh.msg_iovlen = 1;
            mh.msg_control = u.buf; mh.msg_controllen = sizeof u.buf;
            if (recvmsg(sv[1], &mh, 0) != 1) break;
            struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
            if (!c || c->cmsg_type != SCM_RIGHTS) break;
            int rfd;
            memcpy(&rfd, CMSG_DATA(c), sizeof(int));
            uint64_t v = 0;
            if (read(rfd, &v, 8) == 8 && v == (uint64_t)i + 1) good++;
            close(rfd);
        }
        if (ok) { pthread_join(th, NULL); close(sv[0]); close(sv[1]); }
        check(good == 200, "unix: 200 fds passed from another thread, each one works");
    }
    {   /* a recv asleep on a stream socket ends (EOF) when the peer is closed */
        int sv[2];
        pthread_t th;
        int ok = socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
        g_us_close_fd = ok ? sv[0] : -1;
        if (ok) pthread_create(&th, NULL, us_late_closer, NULL);
        char c;
        long t0 = now_ms();
        ssize_t r = ok ? recv(sv[1], &c, 1, 0) : -1;
        long dt = now_ms() - t0;
        if (ok) { pthread_join(th, NULL); close(sv[1]); }
        check(r == 0 && dt < 1500, "unix: closing the peer wakes a sleeping recv (EOF)");
    }
    {   /* the clock: monotonic, finer than a millisecond */
        struct timespec a, b;
        int fine = 0, back = 0;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int i = 0; i < 20000; i++) {
            clock_gettime(CLOCK_MONOTONIC, &b);
            if (b.tv_sec < a.tv_sec || (b.tv_sec == a.tv_sec && b.tv_nsec < a.tv_nsec)) back++;
            if (b.tv_nsec % 1000000 != 0) fine++;
            a = b;
        }
        check(back == 0, "clock_gettime: monotonic");
        check(fine > 0, "clock_gettime: sub-millisecond resolution");
    }
    {   /* a signal from another thread ends a 5 s nanosleep at once, also
           when sent the moment the sleeper starts (no wake-up lost) */
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_usr1;
        sigaction(SIGUSR1, &sa, NULL);
        int ok = 1;
        for (int i = 0; i < 20 && ok; i++) {
            pthread_t th;
            long took = -2;
            g_sleep_ready = 0;
            pthread_create(&th, NULL, long_sleeper, &took);
            while (!g_sleep_ready) sched_yield();
            if (i & 1) { struct timespec n = { 0, 20000000 }; nanosleep(&n, NULL); }
            pthread_kill(th, SIGUSR1);
            pthread_join(th, NULL);
            if (took < 0 || took > 2000) {
                fprintf(stderr, "[forktest] sleep %d: %ld ms\n", i, took);
                ok = 0;
            }
        }
        signal(SIGUSR1, SIG_DFL);
        check(ok, "nanosleep: a signal from another thread wakes it (20 times)");
    }
    /* re-exec'd by the posix_spawn test */
    if (argc > 1 && strcmp(argv[1], "child5") == 0) return 5;

    int st;
    pid_t p;

    signal(SIGCHLD, on_sigchld);

    /* 1. fork + _exit code */
    p = fork();
    if (p == 0) _exit(42);
    check(p > 0 && waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 42,
          "fork + waitpid: exit status 42");

    /* 2. death by signal */
    p = fork();
    if (p == 0) { raise(SIGTERM); _exit(0); }
    check(waitpid(p, &st, 0) == p && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM,
          "child killed by SIGTERM -> WIFSIGNALED");

    /* 3. WNOHANG on a running child, then ECHILD once all are reaped */
    p = fork();
    if (p == 0) { usleep(300 * 1000); _exit(1); }
    check(waitpid(p, &st, WNOHANG) == 0, "WNOHANG on a running child returns 0");
    check(waitpid(p, &st, 0) == p && WEXITSTATUS(st) == 1, "then waitpid collects it");
    check(waitpid(-1, &st, WNOHANG) == -1 && errno == ECHILD, "no children left -> ECHILD");

    /* 4. vfork: the child runs in our memory, we resume after it exits */
    volatile int shared = 0;
    p = vfork();
    if (p == 0) { shared = 1234; _exit(7); }
    check(shared == 1234, "vfork child wrote into the parent's memory");
    check(waitpid(p, &st, 0) == p && WEXITSTATUS(st) == 7, "vfork child exit status 7");

    /* 5. posix_spawn: exec failure must come back as an error */
    char *bad_argv[] = { "/no/such/program", NULL };
    int r = posix_spawn(&p, "/no/such/program", NULL, NULL, bad_argv, environ);
    check(r == ENOENT, "posix_spawn of a missing program returns ENOENT");

    /* 6. posix_spawn of a real program (ourselves) with an exit code */
    char *ok_argv[] = { argv[0], "child5", NULL };
    r = posix_spawn(&p, argv[0], NULL, NULL, ok_argv, environ);
    check(r == 0 && waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 5,
          "posix_spawn + waitpid: exit status 5");

    /* 7. exit() from a helper thread ends the whole process */
    p = fork();
    if (p == 0) {
        pthread_t th;
        pthread_create(&th, NULL, exit_from_thread, NULL);
        sleep(30);  /* would outlive the test if exit_group left us running */
        _exit(99);
    }
    long t0 = now_ms();
    check(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 3 && now_ms() - t0 < 10000,
          "exit() in a helper thread ends the process (status 3)");

    /* 8. kill -9 a child spinning in user code (no syscalls) */
    p = fork();
    if (p == 0) { for (volatile unsigned long i = 0;; i++) {} }
    usleep(100 * 1000);
    check(kill(p, SIGKILL) == 0 && waitpid(p, &st, 0) == p && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
          "kill -9 of a busy-looping child");

    /* 9. waitid + sessions/groups */
    p = fork();
    if (p == 0) {
        pid_t sid = setsid();
        _exit(sid == getpid() && getpgid(0) == getpid() && getsid(0) == getpid() ? 0 : 1);
    }
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    check(waitid(P_PID, p, &si, WEXITED) == 0 && si.si_pid == p && si.si_code == CLD_EXITED && si.si_status == 0,
          "waitid + setsid/getpgid/getsid in the child");
    check(getpgid(0) == getpgrp() && kill(getpid(), 0) == 0, "getpgrp and kill(self, 0)");
    check(kill(999999, 0) == -1 && errno == ESRCH, "kill of a missing pid -> ESRCH");

    /* 10. SIGCHLD reached our handler */
    check(g_sigchld > 0, "SIGCHLD handler ran");

    {
        static char out[4096];
        pid_t q = fork();
        if (q == 0) {
            char *av[] = { "/usr/bin/curl", "--version", NULL };
            char *ev[] = { "TERM=vt100", "CURL_CA_BUNDLE=/etc/ssl/cert.pem", NULL };
            execve(av[0], av, ev);
            _exit(127);
        }
        int qs = 0;
        waitpid(q, &qs, 0);
        fprintf(stderr, "[forktest] curl to console: status=0x%x\n", qs);
        int rc = run_capture("/usr/bin/curl", "--version", NULL, out, sizeof(out));
        fprintf(stderr, "[forktest] curl rc=%d out=%.80s\n", rc, out);
        check(rc == 0 && strncmp(out, "curl ", 5) == 0, "fork+execve /usr/bin/curl --version");
        rc = run_capture("/usr/bin/nano", "--version", NULL, out, sizeof(out));
        fprintf(stderr, "[forktest] nano rc=%d out=%.80s\n", rc, out);
        check(rc == 0 && strstr(out, "nano") != NULL, "fork+execve /usr/bin/nano --version");
        /* wynrc's control socket answers: rc-status lists the services */
        rc = run_capture("/usr/bin/rc-status", NULL, NULL, out, sizeof(out));
        fprintf(stderr, "[forktest] rc-status rc=%d out=%.120s\n", rc, out);
        check(rc == 0 && strstr(out, "SERVICE") && strstr(out, "forktest"), "rc-status talks to wynrc");
        /* wynrc's control: not root, so only this test's own service; a
           service file from outside /etc/wynrc/services used to run as root */
        {
            FILE *sf = fopen("/tmp/rcx", "w");
            if (sf) { fputs("type=oneshot\nexec=/usr/bin/touch /tmp/rcx-ran\n", sf); fclose(sf); }
            unlink("/tmp/rcx-ran");
            run_capture("/usr/bin/rc-service", "../../../tmp/rcx", "start", out, sizeof(out));
            fprintf(stderr, "[forktest] rc-service ../tmp start: %.80s\n", out);
            bool bad_name = strstr(out, "no service") != NULL;
            run_capture("/usr/bin/rc-service", "testuser", "restart", out, sizeof(out));
            fprintf(stderr, "[forktest] rc-service testuser restart: %.80s\n", out);
            bool denied = strstr(out, "permission denied") != NULL;
            run_capture("/usr/bin/rc-service", "forktest", "start", out, sizeof(out));
            fprintf(stderr, "[forktest] rc-service forktest start: %.80s\n", out);
            bool own = strstr(out, "ok:") != NULL;
            usleep(500000);
            check(bad_name && access("/tmp/rcx-ran", F_OK) != 0, "wynrc refuses a service outside its directory");
            check(denied && own, "wynrc: root's services refused, its own allowed");
        }
        /* the display server's devices are root's: a user reads no keys,
           no pointer, no screen */
        {
            int k = open("/dev/input/kbd", O_RDONLY), ke = errno;
            int m = open("/dev/input/mice", O_RDONLY), me = errno;
            int f = open("/dev/fb0", O_RDWR), fe = errno;
            long mv[3 * 4];
            long r = syscall(412, mv, 4, 0), re = errno;
            check(k < 0 && ke == EACCES && m < 0 && me == EACCES && f < 0 && fe == EACCES,
                  "keyboard, pointer, framebuffer: not for users (EACCES)");
            check(r < 0 && re == EPERM, "the pointer stream: not without /dev/input/mice");
            if (k >= 0) close(k);
            if (m >= 0) close(m);
            if (f >= 0) close(f);
        }
        /* dup() takes the lowest free fd: close(0); dup(p) is stdin = p
           (groff's pipeline; it used to get 3 and up, never 0-2) */
        {
            int p[2];
            pid_t c = pipe(p) == 0 ? fork() : -1;
            if (c == 0) {
                close(0);
                int d = dup(p[0]);
                close(1);
                int e = dup(p[1]);
                _exit(d == 0 && e == 1 && write(1, "x", 1) == 1 ? 0 : 1);
            }
            int cs = -1;
            if (c > 0) waitpid(c, &cs, 0);
            char ch = 0;
            bool got = c > 0 && read(p[0], &ch, 1) == 1 && ch == 'x';
            check(c > 0 && WIFEXITED(cs) && WEXITSTATUS(cs) == 0 && got, "close(0); dup(): the lowest free fd");
            close(p[0]); close(p[1]);
            unlink("/tmp/rcx");
        }
        /* audit regressions: each of these used to reach kernel memory */
        {
            /* C1: /dev/fb0 maps the framebuffer and not one page more */
            int fb = open("/dev/fb0", O_RDWR);
            if (fb >= 0) {
                void *m = mmap(NULL, 1UL << 30, PROT_READ, MAP_SHARED, fb, 0);
                check(m == MAP_FAILED, "mmap(/dev/fb0, 1 GB) refused");
                if (m != MAP_FAILED) munmap(m, 1UL << 30);
                close(fb);
            }
            /* C3: MAP_FIXED / munmap / mprotect over the kernel (its image,
               at the top of the address space) */
            void *const kimg = (void *)0xFFFFFFFF80100000UL;
            void *k = mmap(kimg, 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            check(k == MAP_FAILED, "MAP_FIXED over kernel memory refused");
            check(munmap(kimg, 4096) != 0, "munmap of kernel memory refused");
            check(mprotect(kimg, 4096, PROT_READ) != 0, "mprotect of kernel memory refused");
            long t0 = now_ms();
            munmap((void *)0x8000000000UL, 1UL << 46);   /* huge, empty: must not hang */
            check(now_ms() - t0 < 1000, "munmap of a huge empty range returns at once");
            /* mremap copying from a kernel address */
            void *r = (void *)syscall(SYS_mremap, kimg, 4096UL, 8192UL, 1UL, 0UL);
            check(r == MAP_FAILED, "mremap from kernel memory refused");
            /* C5: a signal handler outside user space */
            struct { unsigned long h, f, r, m; } sa = { 0xffff800000001000UL, 0x04000000UL, 0, 0 };
            check(syscall(SYS_rt_sigaction, SIGUSR2, &sa, NULL, 8) != 0, "sigaction with a kernel handler refused");
            /* H1: empty datagrams can't fill the kernel heap */
            int sv[2];
            if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, sv) == 0) {
                int n = 0;
                while (n < 100000 && send(sv[0], "", 0, 0) == 0) n++;
                check(n < 100000 && errno == EAGAIN, "AF_UNIX queue bounded");
                fprintf(stderr, "[forktest] AF_UNIX queue held %d empty datagrams\n", n);
                close(sv[0]); close(sv[1]);
            }
            /* randomness: getrandom() and /dev/urandom were TSC words / zeros */
            {
                unsigned char r1[32] = {0}, r2[32] = {0}, u[32] = {0};
                syscall(SYS_getrandom, r1, sizeof r1, 0);
                syscall(SYS_getrandom, r2, sizeof r2, 0);
                int fd = open("/dev/urandom", O_RDONLY);
                if (fd >= 0) { read(fd, u, sizeof u); close(fd); }
                int zeros = 0;
                for (int i = 0; i < 32; i++) zeros += (u[i] == 0);
                check(memcmp(r1, r2, sizeof r1) != 0, "getrandom gives different bytes each call");
                check(zeros < 8, "/dev/urandom is not zeros");
            }
            /* demand paging: reservations cost nothing until touched */
            {
                struct sysinfo si0, si1;
                void *res = mmap(NULL, 16UL << 30, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
                check(res != MAP_FAILED, "16 GB PROT_NONE reservation");
                sysinfo(&si0);
                char *big = mmap(NULL, 1UL << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                check(big != MAP_FAILED, "1 GB anonymous mapping");
                if (big != MAP_FAILED) {
                    big[0] = 1; big[(1UL << 30) - 1] = 2;   /* two pages touched */
                    sysinfo(&si1);
                    long used = (long)(si0.freeram - si1.freeram);
                    fprintf(stderr, "[forktest] 1 GB mapping, 2 pages touched: %ld KB used\n", used / 1024);
                    check(used < 4L * 1024 * 1024, "untouched pages cost no RAM");
                    check(big[12345] == 0 && big[1UL << 29] == 0, "fresh pages read as zero");
                    /* the kernel writing into a never-touched page (read() from a pipe) */
                    int pp[2];
                    if (pipe(pp) == 0) {
                        write(pp[1], "lazy", 4);
                        check(read(pp[0], big + (300UL << 20), 4) == 4 && memcmp(big + (300UL << 20), "lazy", 4) == 0,
                              "read() into an untouched page");
                        close(pp[0]); close(pp[1]);
                    }
                    munmap(big, 1UL << 30);
                }
                if (res != MAP_FAILED) {
                    /* touching PROT_NONE must still kill: in a child */
                    pid_t c = fork();
                    if (c == 0) { *(volatile char *)res = 1; _exit(0); }
                    int st = 0;
                    waitpid(c, &st, 0);
                    check(WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st) != 0), "PROT_NONE page still faults");
                    munmap(res, 16UL << 30);
                }
            }
            /* filesystem: a file past the doubly-indirect boundary (the table
               used to be wiped when it grew another level), O_TRUNC keeping
               the mode, sync() */
            {
                const char *big = "/tmp/forktest-big";
                int fd = open(big, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                static unsigned int chunk[16384];   /* 64 KB */
                int ok = fd >= 0;
                for (int k = 0; ok && k < 160; k++) {          /* 10 MB */
                    for (int j = 0; j < 16384; j++) chunk[j] = (unsigned)(k * 16384 + j) * 2654435761u;
                    ok = write(fd, chunk, sizeof chunk) == (ssize_t)sizeof chunk;
                }
                if (fd >= 0) close(fd);
                check(ok, "write a 10 MB file");
                check(syscall(SYS_sync) == 0, "sync()");
                fd = open(big, O_RDONLY);
                int good = fd >= 0;
                for (int k = 0; good && k < 160; k++) {
                    good = read(fd, chunk, sizeof chunk) == (ssize_t)sizeof chunk;
                    for (int j = 0; good && j < 16384; j++)
                        good = chunk[j] == (unsigned)(k * 16384 + j) * 2654435761u;
                }
                if (fd >= 0) close(fd);
                check(good, "10 MB file reads back intact");
                unlink(big);

                const char *sh = "/tmp/forktest-sh";
                fd = open(sh, O_WRONLY | O_CREAT, 0644);
                if (fd >= 0) { write(fd, "#!/bin/sh\n", 10); close(fd); }
                chmod(sh, 0755);
                fd = open(sh, O_WRONLY | O_TRUNC);
                if (fd >= 0) close(fd);
                struct stat ts;
                check(stat(sh, &ts) == 0 && (ts.st_mode & 0777) == 0755 && ts.st_size == 0,
                      "O_TRUNC keeps the mode");
                unlink(sh);
            }
            /* review regressions */
            {
                /* a failed execve() leaves the lazy memory of the image usable */
                char *lazy = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                pid_t c = fork();
                if (c == 0) {
                    char *na[] = { "/nonexistent", NULL };
                    execve("/nonexistent", na, environ);
                    lazy[12345] = 7;                       /* untouched until now */
                    _exit(lazy[12345] == 7 ? 0 : 1);
                }
                int st = 0;
                waitpid(c, &st, 0);
                check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "failed execve keeps lazy memory usable");
                munmap(lazy, 1 << 20);

                /* a mapped file outlives its name */
                const char *mf = "/tmp/forktest-mapped";
                int fd = open(mf, O_RDWR | O_CREAT | O_TRUNC, 0644);
                static unsigned char page[4096];
                for (int k = 0; k < 64; k++) {             /* 256 KB, page k filled with k */
                    memset(page, k, sizeof page);
                    if (write(fd, page, sizeof page) != (ssize_t)sizeof page) break;
                }
                unsigned char *m = mmap(NULL, 64 * 4096, PROT_READ, MAP_PRIVATE, fd, 0);
                close(fd);
                unlink(mf);
                /* and a new file that may take the freed inode */
                int fd2 = open("/tmp/forktest-other", O_RDWR | O_CREAT | O_TRUNC, 0644);
                memset(page, 0xEE, sizeof page);
                for (int k = 0; k < 64; k++) if (write(fd2, page, sizeof page) < 0) break;
                close(fd2);
                int intact = m != MAP_FAILED;
                for (int k = 0; intact && k < 64; k++) intact = m[k * 4096 + 100] == k;
                check(intact, "unlinked mapped file still reads its own data");
                if (m != MAP_FAILED) munmap(m, 64 * 4096);
                unlink("/tmp/forktest-other");

                /* an open file outlives its name too: created, unlinked, then
                   sized, written, mapped and read through its fd (mkstemp +
                   unlink, as Qt makes the Wayland keymap) -- and a dup of
                   the fd keeps it after the first is closed */
                char tmpl[] = "/tmp/forktest-anonXXXXXX";
                int af = mkstemp(tmpl);
                unlink(tmpl);
                int okf = af >= 0 && ftruncate(af, 8192) == 0 && pwrite(af, "keymap", 7, 4096) == 7;
                int af2 = okf ? dup(af) : -1;
                if (af >= 0) close(af);
                unsigned char *am = af2 >= 0 ? mmap(NULL, 8192, PROT_READ, MAP_PRIVATE, af2, 0) : MAP_FAILED;
                okf = okf && am != MAP_FAILED && !memcmp(am + 4096, "keymap", 7);
                if (am != MAP_FAILED) munmap(am, 8192);
                if (af2 >= 0) close(af2);
                check(okf, "unlinked open file still usable through its fds");

                /* MAP_SHARED of a file is shared: code written through one
                   mapping shows in another (GStreamer's orc JIT maps its
                   file RW and RX), in a forked child, and in the file after
                   msync() */
                char shp[] = "/tmp/forktest-sharedXXXXXX";
                int sf = mkstemp(shp);
                unlink(shp);
                int oks = sf >= 0 && ftruncate(sf, 8192) == 0;
                unsigned char *rw = oks ? mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, sf, 0) : MAP_FAILED;
                unsigned char *ro = oks ? mmap(NULL, 8192, PROT_READ, MAP_SHARED, sf, 0) : MAP_FAILED;
                oks = oks && rw != MAP_FAILED && ro != MAP_FAILED;
                if (oks) {
                    memcpy(rw + 100, "jit code", 9);
                    check(!memcmp(ro + 100, "jit code", 9), "MAP_SHARED: two mappings of a file see each other");
                    pid_t sc = fork();
                    if (sc == 0) { memcpy(rw + 4096, "from child", 11); _exit(0); }
                    int sst;
                    waitpid(sc, &sst, 0);
                    check(!memcmp(ro + 4096, "from child", 11), "MAP_SHARED: a child's writes reach the parent");
                    char fb[16] = {0};
                    check(msync(rw, 8192, MS_SYNC) == 0 && pread(sf, fb, 9, 100) == 9 && !strcmp(fb, "jit code"),
                          "MAP_SHARED: msync() writes the file");
                } else {
                    check(0, "MAP_SHARED: two mappings of a file see each other");
                }
                if (rw != MAP_FAILED) munmap(rw, 8192);
                if (ro != MAP_FAILED) munmap(ro, 8192);
                if (sf >= 0) close(sf);
                int rof = open("/etc/hosts", O_RDONLY);
                void *bad = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, rof, 0);
                check(bad == MAP_FAILED && errno == EACCES, "MAP_SHARED writable needs a writable fd");
                if (bad != MAP_FAILED) munmap(bad, 4096);
                close(rof);
            }
            /* normal anonymous mappings still work */
            char *a = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            check(a != MAP_FAILED, "anonymous mmap still works");
            if (a != MAP_FAILED) {
                a[0] = 1; a[(1 << 20) - 1] = 2;
                check(munmap(a, 1 << 20) == 0, "munmap of own mapping works");
            }
        }
        /* permissions: a user (the test account, tester) can't write /etc,
           can write /tmp and its home, can't read /etc/shadow, can't
           setuid(0) */
        if (getuid() != 0) {
            const struct passwd *me = getpwuid(getuid());
            char home[256], probe[300];
            snprintf(home, sizeof home, "%s", me ? me->pw_dir : "/nonexistent");
            snprintf(probe, sizeof probe, "%s/.forktest-perm", home);
            FILE *f = fopen("/etc/forktest-perm", "w");
            check(f == NULL, "user cannot create files in /etc");
            if (f) { fclose(f); unlink("/etc/forktest-perm"); }
            f = fopen("/tmp/forktest-perm", "w");
            check(f != NULL, "user can create files in /tmp");
            if (f) { fclose(f); check(unlink("/tmp/forktest-perm") == 0, "user can delete its file in /tmp"); }
            f = fopen(probe, "w");
            check(f != NULL, "user can create files in its home");
            if (f) { fclose(f); unlink(probe); }
            check(unlink("/etc/hosts") != 0 && access("/etc/hosts", F_OK) == 0, "user cannot delete /etc files");
            check(mkdir("/tmp/forktest-d", 0755) == 0, "mkdir in /tmp");
            f = fopen("/tmp/forktest-d/a", "w");
            if (f) fclose(f);
            check(rename("/tmp/forktest-d/a", "/tmp/forktest-d/b") == 0 &&
                  access("/tmp/forktest-d/b", F_OK) == 0, "rename");
            check(rmdir("/tmp/forktest-d") != 0 && errno == ENOTEMPTY, "rmdir refuses a non-empty dir");
            unlink("/tmp/forktest-d/b");
            check(rmdir("/tmp/forktest-d") == 0, "rmdir");
            struct stat hs, es;
            check(stat(home, &hs) == 0 && hs.st_uid == getuid() && (hs.st_mode & 0777) == 0700 &&
                  hs.st_mtime > 1600000000, "stat: real owner, mode and mtime");
            check(stat("/etc/hosts", &es) == 0 && es.st_uid == 0 && (es.st_mode & 0777) == 0644, "stat: /etc/hosts root 0644");
            check(stat("/tmp", &es) == 0 && (es.st_mode & 07777) == 01777, "stat: /tmp is 1777");
            check(setuid(0) != 0, "user cannot setuid(0)");
            check(syscall(409, "definitely-wrong") != 0, "ary elevate rejects a wrong password");
            check(getuid() != 0, "still a user afterwards");
        }
        /* `ary` end to end -- only with BOOT_EXTRA=authtest=1 and QEMU_EXTRA=-snapshot,
           since it sets a real root password on the disk */
        char cfg[512] = {0};
        FILE *cf = fopen("/etc/wynland/boot.cfg", "r");
        if (cf) { fread(cfg, 1, sizeof(cfg) - 1, cf); fclose(cf); }
        if (strstr(cfg, "authtest=1") && getuid() != 0) {
            check(syscall(414) == 0, "ary: no superuser on a fresh disk");
            check(syscall(409, "x") == -1 && errno == ENOENT, "ary su without a superuser -> ENOENT");
            check(syscall(413, "evil", 0) != 0 && syscall(414) == 0, "a user cannot set the first root password");
            check(access("/etc/shadow", R_OK) != 0, "/etc/shadow unreadable for users");
            check(syscall(409, "evil") != 0 && getuid() != 0, "and so cannot elevate");
        }
        {
            long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
            if (ncpu < 1) ncpu = 1;
            if (ncpu > 8) ncpu = 8;
            int n = (int)(2 * ncpu);
            struct spinner sp[16];
            pthread_t th[16];
            g_spin_stop = 0;
            for (int i = 0; i < n; i++) {
                sp[i].nice = (i & 1) ? 10 : 0;
                sp[i].count = 0;
                pthread_create(&th[i], NULL, spinner, &sp[i]);
            }
            struct timespec d = { 2, 0 };
            nanosleep(&d, NULL);
            g_spin_stop = 1;
            unsigned long hi = 0, lo = 0;
            for (int i = 0; i < n; i++) {
                pthread_join(th[i], NULL);
                if (i & 1) lo += sp[i].count; else hi += sp[i].count;
            }
            fprintf(stderr, "[forktest] eevdf: nice 0 %lu, nice 10 %lu (x%lu)\n", hi, lo, lo ? hi / lo : 0);
            check(lo > 0 && hi > 3 * lo, "scheduler: nice 0 gets several times the CPU of nice 10");
            check(getpriority(PRIO_PROCESS, 0) == 0, "getpriority: the main thread still at nice 0");
        }
        /* real network: DNS (musl resolver -> /etc/resolv.conf), TCP, TLS */
        rc = run_capture("/usr/bin/curl", "-sI", "https://example.com/", out, sizeof(out));
        fprintf(stderr, "[forktest] curl https rc=%d out=%.60s\n", rc, out);
        /* 6 = no DNS, 7 = no route, 28 = timeout: no network, not a bug */
        if (rc == 6 || rc == 7 || rc == 28)
            fprintf(stderr, "[forktest] SKIP curl -sI https://example.com/ (no network, rc=%d)\n", rc);
        else
            check(rc == 0 && strncmp(out, "HTTP/", 5) == 0, "curl -sI https://example.com/");
    }
    fprintf(stderr, "[forktest] DONE pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
