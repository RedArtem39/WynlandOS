/*
 * WynlandOS - wynlogin: the login manager (what SDDM/GDM are elsewhere).
 *
 * A wynrc daemon, root. In a loop:
 *   1. the login screen: Zerp 2.0 in greeter mode (zerp2 --greeter),
 *      run as the unprivileged "greeter" account, talking to us over two
 *      pipes -- it asks, we check (it never sees /etc/shadow);
 *   2. on a good password, the user's session: Zerp 2.0 as that user, with
 *      HOME, USER, SHELL... of its account;
 *   3. when the session ends (log out), the login screen again.
 * With no account yet (first boot) the login screen offers to create one:
 * the first user, an administrator.
 *
 * Greeter protocol, one line each way, fields hex-encoded:
 *   -> LOGIN <user> <password>            <- OK | FAIL <reason>
 *   -> CREATE <user> <full name> <password>   (first boot only)
 * boot.cfg "autologin=NAME" skips the screen (screenshots, tests).
 *
 * Copyright (C) 2026 Red_Artem39. GPL-2.0-only.
 */
#define _GNU_SOURCE
#include "accounts.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdint.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SYS_spawn_as 415
#define ZERP2 "/usr/bin/zerp2"
#define GREETER_USER "greeter"

extern char **environ;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[wynlogin] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* the kernel's spawn of another account (root's): its uid, its group and
   the supplementary groups of /etc/group (wheel...) */
static pid_t spawn_as(const char *path, char *const argv[], char *const envp[], const char *user, uid_t uid, gid_t gid)
{
    struct { uint32_t n; uint32_t g[32]; } groups = { 0, {0} };
    gid_t list[32];
    int n = 32;
    if (user && getgrouplist(user, gid, list, &n) >= 0)
        for (int i = 0; i < n && groups.n < 32; i++) groups.g[groups.n++] = list[i];
    long r = syscall(SYS_spawn_as, path, argv, envp, (long)uid, (long)gid, &groups);
    return r < 0 ? -1 : (pid_t)r;
}

static int unhex(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    while (in[0] && in[1] && n + 1 < cap) {
        unsigned v;
        if (sscanf(in, "%2x", &v) != 1) return -1;
        out[n++] = (char)v;
        in += 2;
    }
    out[n] = 0;
    return (int)n;
}

static char *read_line(int fd, char *buf, size_t cap)
{
    size_t n = 0;
    char c;
    while (n + 1 < cap) {
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) return NULL;
        if (c == '\n') break;
        buf[n++] = c;
    }
    buf[n] = 0;
    return buf;
}

static void reply(int fd, const char *s)
{
    dprintf(fd, "%s\n", s);
}

/* the environment of a session or the greeter: ours (TZ, library paths)
   with the account's own values */
static char **make_env(const struct passwd *pw)
{
    static char *env[64];
    static char bufs[12][320];
    int n = 0, b = 0;
    const char *own[] = { "HOME=", "USER=", "LOGNAME=", "SHELL=", "PATH=", "XDG_RUNTIME_DIR=", "LANG=", "MAIL=" };
    for (char **e = environ; *e && n < 48; e++) {
        int mine = 0;
        for (size_t k = 0; k < sizeof own / sizeof *own; k++)
            if (!strncmp(*e, own[k], strlen(own[k]))) mine = 1;
        if (!mine) env[n++] = *e;
    }
    char rt[128];
    snprintf(rt, sizeof rt, "/tmp/runtime-%u", pw->pw_uid);
    mkdir(rt, 0700);
    chown(rt, pw->pw_uid, pw->pw_gid);
    snprintf(bufs[b], sizeof bufs[b], "HOME=%s", pw->pw_dir);          env[n++] = bufs[b++];
    snprintf(bufs[b], sizeof bufs[b], "USER=%s", pw->pw_name);         env[n++] = bufs[b++];
    snprintf(bufs[b], sizeof bufs[b], "LOGNAME=%s", pw->pw_name);      env[n++] = bufs[b++];
    snprintf(bufs[b], sizeof bufs[b], "SHELL=%s", pw->pw_shell);       env[n++] = bufs[b++];
    snprintf(bufs[b], sizeof bufs[b], "XDG_RUNTIME_DIR=%s", rt);       env[n++] = bufs[b++];
    env[n++] = "PATH=/usr/local/bin:/usr/bin:/usr/sbin:/sbin";
    env[n++] = "LANG=C.UTF-8";
    env[n] = NULL;
    return env;
}

static void run_session(const char *user)
{
    struct passwd *pw = getpwnam(user);
    if (!pw) { logf_("no account %s", user); return; }
    char **env = make_env(pw);
    char *argv[] = { ZERP2, NULL };
    pid_t pid = spawn_as(ZERP2, argv, env, user, pw->pw_uid, pw->pw_gid);
    if (pid < 0) { logf_("could not start the session of %s", user); sleep(2); return; }
    logf_("session of %s (uid %u): pid %d", user, pw->pw_uid, (int)pid);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    logf_("session of %s ended (%s %d)", user, WIFEXITED(st) ? "exit" : "signal",
          WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
}

/* the login screen until someone logs in: their name, or "" */
static void greet(char *who, size_t cap)
{
    who[0] = 0;
    struct passwd *g = getpwnam(GREETER_USER);
    uid_t guid = g ? g->pw_uid : 990;
    gid_t ggid = g ? g->pw_gid : 990;
    int first = acct_count_users() == 0;
    int req[2], rep[2];
    if (pipe(req) < 0 || pipe(rep) < 0) { sleep(1); return; }
    fcntl(req[0], F_SETFD, FD_CLOEXEC);
    fcntl(rep[1], F_SETFD, FD_CLOEXEC);
    char a1[16], a2[16];
    snprintf(a1, sizeof a1, "%d", req[1]);
    snprintf(a2, sizeof a2, "%d", rep[0]);
    char *argv[] = { ZERP2, "--greeter", a1, a2, first ? "--first" : NULL, NULL };
    struct passwd fake = { .pw_name = GREETER_USER, .pw_dir = "/var/lib/greeter", .pw_shell = "/bin/false",
                           .pw_uid = guid, .pw_gid = ggid };
    char **env = make_env(g ? g : &fake);
    pid_t pid = spawn_as(ZERP2, argv, env, g ? GREETER_USER : NULL, guid, ggid);
    close(req[1]);
    close(rep[0]);
    if (pid < 0) { logf_("could not start the login screen"); close(req[0]); close(rep[1]); sleep(2); return; }
    logf_("login screen: pid %d%s", (int)pid, first ? " (first boot: create the first account)" : "");

    char line[1024];
    while (read_line(req[0], line, sizeof line)) {
        char f1[128], f2[256], f3[256];
        char user[64] = "", full[128] = "", pwd[256] = "";
        int n = sscanf(line, "%*s %127s %255s %255s", f1, f2, f3);
        if (!strncmp(line, "LOGIN ", 6) && n >= 2) {
            unhex(f1, user, sizeof user);
            unhex(f2, pwd, sizeof pwd);
            struct passwd *pw = getpwnam(user);
            bool ok = pw && pw->pw_uid >= ACCT_FIRST_UID && acct_check_password(user, pwd);
            explicit_bzero(pwd, sizeof pwd);
            if (!ok) { logf_("login of '%s' refused", user); reply(rep[1], "FAIL wrong name or password"); continue; }
            snprintf(who, cap, "%s", user);
            reply(rep[1], "OK");
            break;
        }
        if (!strncmp(line, "CREATE ", 7) && n >= 3) {
            unhex(f1, user, sizeof user);
            unhex(f2, full, sizeof full);
            unhex(f3, pwd, sizeof pwd);
            if (acct_count_users() != 0) { reply(rep[1], "FAIL there is an account already"); continue; }
            int r = acct_add_user(user, full, true);
            if (r == 0) r = acct_set_password(user, pwd);
            explicit_bzero(pwd, sizeof pwd);
            if (r < 0) {
                char msg[128];
                snprintf(msg, sizeof msg, "FAIL %s", r == -EINVAL ? "letters, digits, _ . - only, starting with a letter"
                                                     : r == -EEXIST ? "that name is taken" : strerror(-r));
                reply(rep[1], msg);
                continue;
            }
            logf_("first account: %s (administrator)", user);
            snprintf(who, cap, "%s", user);
            reply(rep[1], "OK");
            break;
        }
        reply(rep[1], "FAIL ?");
    }
    close(req[0]);
    close(rep[1]);
    /* the greeter fades out and exits by itself; make sure */
    for (int i = 0; i < 50; i++) {
        if (waitpid(pid, NULL, WNOHANG) == pid) return;
        usleep(100000);
    }
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
}

static void autologin_name(char *out, size_t cap)
{
    out[0] = 0;
    FILE *f = fopen("/etc/wynland/boot.cfg", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "autologin=", 10)) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(out, cap, "%s", line + 10);
        }
    fclose(f);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    if (geteuid() != 0) { logf_("must run as root"); return 1; }
    char auto_user[64];
    autologin_name(auto_user, sizeof auto_user);
    for (;;) {
        char who[64];
        if (auto_user[0] && getpwnam(auto_user)) {
            snprintf(who, sizeof who, "%s", auto_user);
            auto_user[0] = 0;                   /* once: logging out shows the screen */
        } else {
            greet(who, sizeof who);
        }
        if (who[0]) run_session(who);
    }
}
