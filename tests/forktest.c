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
