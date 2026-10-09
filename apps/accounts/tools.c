/*
 * WynlandOS - useradd, userdel, passwd (one program, named by argv[0]).
 *
 *   useradd [-a] [-c "Full Name"] [--password-stdin] NAME
 *            -a: an administrator (in wheel: may use ary)
 *   userdel [-r] NAME           -r: its home too
 *   passwd [NAME]               your password; root: anyone's
 *   passwd --stdin [NAME]       the new password from stdin (root)
 *
 * passwd is setuid root (a user changes its own after giving the current
 * one); useradd and userdel are root's.
 *
 * Copyright (C) 2026 Red_Artem39. GPL-2.0-or-later.
 */
#define _GNU_SOURCE
#include "accounts.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static int g_stdin;

static void ask(const char *prompt, char *buf, size_t cap)
{
    /* the terminal: stdin when it is one (a PTY without a controlling
       terminal, as in Zerp's terminal), else /dev/tty */
    int fd = (g_stdin || isatty(0)) ? 0 : open("/dev/tty", O_RDWR);
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
}

static int new_password(const char *user)
{
    char a[256], b[256];
    ask("new password: ", a, sizeof a);
    if (!g_stdin) ask("again: ", b, sizeof b); else strcpy(b, a);
    int rc = 1;
    if (!a[0]) fprintf(stderr, "passwd: an empty password is not allowed\n");
    else if (strcmp(a, b)) fprintf(stderr, "passwd: the passwords differ, nothing changed\n");
    else {
        int r = acct_set_password(user, a);
        if (r == 0) rc = 0;
        else fprintf(stderr, "passwd: %s\n", strerror(-r));
    }
    explicit_bzero(a, sizeof a);
    explicit_bzero(b, sizeof b);
    return rc;
}

static int need_root(const char *prog)
{
    if (geteuid() != 0) { fprintf(stderr, "%s: only root may do this (ary %s ...)\n", prog, prog); return 1; }
    return 0;
}

static int cmd_useradd(int argc, char **argv)
{
    if (need_root("useradd")) return 1;
    int admin = 0;
    const char *full = "", *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a") || !strcmp(argv[i], "--admin")) admin = 1;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) full = argv[++i];
        else if (!strcmp(argv[i], "--password-stdin")) g_stdin = 1;
        else name = argv[i];
    }
    if (!name) { fprintf(stderr, "usage: useradd [-a] [-c \"Full Name\"] [--password-stdin] NAME\n"); return 2; }
    int r = acct_add_user(name, full, admin);
    if (r < 0) {
        fprintf(stderr, "useradd: %s\n", r == -EEXIST ? "that name is taken" :
                r == -EINVAL ? "not a valid name (a letter, then letters, digits, _ . -)" : strerror(-r));
        return 1;
    }
    struct passwd *pw = getpwnam(name);
    printf("useradd: %s (uid %u)%s\n", name, pw ? pw->pw_uid : 0, admin ? ", administrator" : "");
    if (g_stdin) return new_password(name);
    return 0;
}

static int cmd_userdel(int argc, char **argv)
{
    if (need_root("userdel")) return 1;
    int home = 0;
    const char *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r")) home = 1;
        else name = argv[i];
    }
    if (!name) { fprintf(stderr, "usage: userdel [-r] NAME\n"); return 2; }
    int r = acct_del_user(name, home);
    if (r < 0) { fprintf(stderr, "userdel: %s\n", r == -ENOENT ? "no such user" : strerror(-r)); return 1; }
    return 0;
}

static int cmd_passwd(int argc, char **argv)
{
    const char *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--stdin")) g_stdin = 1;
        else name = argv[i];
    }
    struct passwd *me = getpwuid(getuid());
    if (!me) { fprintf(stderr, "passwd: who are you?\n"); return 1; }
    char self[64];
    snprintf(self, sizeof self, "%s", me->pw_name);
    if (!name) name = self;
    if (getuid() != 0) {
        /* a user: only its own, and only with the current one */
        if (strcmp(name, self)) { fprintf(stderr, "passwd: you may change only your own password\n"); return 1; }
        if (g_stdin) { fprintf(stderr, "passwd: --stdin is root's\n"); return 1; }
        char cur[256];
        ask("current password: ", cur, sizeof cur);
        bool ok = acct_check_password(name, cur);
        explicit_bzero(cur, sizeof cur);
        if (!ok) { fprintf(stderr, "passwd: wrong password, nothing changed\n"); return 1; }
    } else if (!getpwnam(name)) {
        fprintf(stderr, "passwd: no user %s\n", name);
        return 1;
    }
    if (new_password(name)) return 1;
    printf("passwd: the password of %s is changed\n", name);
    return 0;
}

int main(int argc, char **argv)
{
    const char *prog = basename(argv[0]);
    if (!strcmp(prog, "useradd")) return cmd_useradd(argc, argv);
    if (!strcmp(prog, "userdel")) return cmd_userdel(argc, argv);
    if (!strcmp(prog, "passwd")) return cmd_passwd(argc, argv);
    fprintf(stderr, "run me as useradd, userdel or passwd\n");
    return 2;
}
