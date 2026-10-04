/*
 * WynlandOS - ary: become root (sudo/su in one).
 *
 *   ary CMD [ARGS...]   run one command as root
 *   ary su              a root shell ($SHELL, else fish)
 *   ary login           create the superuser: set the root password (once)
 *   ary passwd          change the root password
 *   ary -S ...          read the password(s) from stdin, not the terminal
 *
 * The kernel checks the password (SYS_wynland_elevate, against the salted
 * hash in /etc/shadow) and makes the calling process root in place; what
 * it then execs inherits uid 0. Zerp 1's terminal has ary built in; this
 * is the same for every other shell.
 *
 * Copyright (C) 2026 Red_Artem39. GPL-2.0-only.
 * Build: gcc -O2 -o build/ary apps/ary/ary.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <termios.h>
#include <unistd.h>

#define SYS_wynland_elevate 409
#define SYS_ary_passwd      413
#define SYS_ary_status      414

static int g_stdin;   /* -S */

/* one line, without echo when it comes from the terminal */
static int ask(const char *prompt, char *buf, size_t cap)
{
    int fd = g_stdin ? 0 : open("/dev/tty", O_RDWR);
    if (fd < 0) fd = 0;
    struct termios old, quiet;
    int tty = !g_stdin && tcgetattr(fd, &old) == 0;
    if (tty) {
        dprintf(fd, "%s", prompt);
        quiet = old;
        quiet.c_lflag &= ~(ECHO | ECHOE | ECHOK);
        quiet.c_lflag |= ECHONL;
        tcsetattr(fd, TCSAFLUSH, &quiet);
    }
    size_t n = 0;
    char ch;
    while (n + 1 < cap && read(fd, &ch, 1) == 1 && ch != '\n') buf[n++] = ch;
    buf[n] = 0;
    if (tty) tcsetattr(fd, TCSAFLUSH, &old);
    if (fd > 2) close(fd);
    return (int)n;
}

static void wipe(char *s, size_t n) { explicit_bzero(s, n); }

static int set_password(int have)
{
    char old[128] = "", a[128], b[128];
    if (have && getuid() != 0) ask("current root password: ", old, sizeof old);
    ask("new root password: ", a, sizeof a);
    ask("again: ", b, sizeof b);
    int rc = 1;
    if (!a[0]) fprintf(stderr, "ary: an empty password is not allowed\n");
    else if (strcmp(a, b)) fprintf(stderr, "ary: the passwords differ, nothing changed\n");
    else {
        long r = syscall(SYS_ary_passwd, a, old[0] ? old : NULL);
        if (r == 0) { printf("ary: root password set -- 'ary su' or 'ary CMD' to use it\n"); rc = 0; }
        else fprintf(stderr, "ary: %s\n", errno == EACCES ? "wrong current password, nothing changed"
                                                        : "could not save the password");
    }
    wipe(old, sizeof old); wipe(a, sizeof a); wipe(b, sizeof b);
    return rc;
}

static int elevate(void)
{
    if (getuid() == 0) return 0;
    char pw[128];
    ask("[ary] root password: ", pw, sizeof pw);
    long r = syscall(SYS_wynland_elevate, pw);
    wipe(pw, sizeof pw);
    if (r == 0) return 0;
    fprintf(stderr, "ary: %s\n", errno == ENOENT ? "no superuser yet -- run 'ary login' first"
                                                 : "incorrect password");
    return -1;
}

int main(int argc, char **argv)
{
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-S")) { g_stdin = 1; i++; }
    int have = syscall(SYS_ary_status) == 1;
    if (i >= argc || !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
        printf("usage: ary [-S] CMD [ARGS...] | su | login | passwd\n"
               "superuser: %s\n", have ? "set" : "none yet -- 'ary login' creates it");
        return i >= argc ? 1 : 0;
    }
    if (!strcmp(argv[i], "login")) {
        if (have) { fprintf(stderr, "ary: the superuser exists -- 'ary passwd' changes its password\n"); return 1; }
        printf("ary: creating the superuser (root)\n");
        return set_password(0);
    }
    if (!strcmp(argv[i], "passwd")) return set_password(have);
    if (elevate() != 0) return 1;
    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);
    setenv("HOME", "/root", 1);
    if (!strcmp(argv[i], "su")) {
        const char *sh = getenv("SHELL");
        if (!sh || access(sh, X_OK)) sh = "/usr/bin/fish";
        execl(sh, sh, (char *)NULL);
        perror(sh);
        return 127;
    }
    execvp(argv[i], argv + i);
    fprintf(stderr, "ary: %s: %s\n", argv[i], strerror(errno));
    return 127;
}
