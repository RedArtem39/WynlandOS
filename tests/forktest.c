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

    fprintf(stderr, "[forktest] DONE pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
