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
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
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

int main(int argc, char **argv)
{
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
            /* C3: MAP_FIXED / munmap / mprotect over the kernel identity map */
            void *k = mmap((void *)0x200000, 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            check(k == MAP_FAILED, "MAP_FIXED over kernel memory refused");
            check(munmap((void *)0x200000, 4096) != 0, "munmap of kernel memory refused");
            check(mprotect((void *)0x200000, 4096, PROT_READ) != 0, "mprotect of kernel memory refused");
            long t0 = now_ms();
            munmap((void *)0x8000000000UL, 1UL << 46);   /* huge, empty: must not hang */
            check(now_ms() - t0 < 1000, "munmap of a huge empty range returns at once");
            /* the kernel has pages in the user half too (virtio-gpu queues):
               that munmap must have left them alone -- the GPU still works */
            /* mremap copying from a kernel address */
            void *r = (void *)syscall(SYS_mremap, (void *)0x200000UL, 4096UL, 8192UL, 1UL, 0UL);
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
            }
            /* normal anonymous mappings still work */
            char *a = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            check(a != MAP_FAILED, "anonymous mmap still works");
            if (a != MAP_FAILED) {
                a[0] = 1; a[(1 << 20) - 1] = 2;
                check(munmap(a, 1 << 20) == 0, "munmap of own mapping works");
            }
        }
        /* permissions: a user (uid 1000) can't write /etc, can write /tmp
           and its home, can't read /etc/shadow, can't setuid(0) */
        if (getuid() != 0) {
            FILE *f = fopen("/etc/forktest-perm", "w");
            check(f == NULL, "user cannot create files in /etc");
            if (f) { fclose(f); unlink("/etc/forktest-perm"); }
            f = fopen("/tmp/forktest-perm", "w");
            check(f != NULL, "user can create files in /tmp");
            if (f) { fclose(f); check(unlink("/tmp/forktest-perm") == 0, "user can delete its file in /tmp"); }
            f = fopen("/home/user/.forktest-perm", "w");
            check(f != NULL, "user can create files in /home/user");
            if (f) { fclose(f); unlink("/home/user/.forktest-perm"); }
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
            check(stat("/home/user", &hs) == 0 && hs.st_uid == 1000 && (hs.st_mode & 0777) == 0755 &&
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
            check(syscall(413, "s3cret", 0) == 0, "ary login creates the superuser");
            check(syscall(414) == 1, "ary: superuser exists now");
            check(syscall(413, "evil", 0) != 0, "a user cannot overwrite it");
            check(access("/etc/shadow", R_OK) != 0, "/etc/shadow unreadable for users");
            check(syscall(409, "wrong") != 0 && getuid() != 0, "wrong password refused");
            check(syscall(409, "s3cret") == 0 && getuid() == 0, "ary su -> root");
            FILE *f = fopen("/etc/forktest-root", "w");
            check(f != NULL, "root can write /etc");
            if (f) { fclose(f); unlink("/etc/forktest-root"); }
            check(setuid(1000) == 0 && getuid() == 1000, "exit -> back to user");
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
