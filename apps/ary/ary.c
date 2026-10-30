/*
 * WynlandOS - ary: become root (sudo and su in one).
 *
 *   ary CMD [ARGS...]   run one command as root
 *   ary su              a root shell ($SHELL, else fish)
 *   ary -S ...          the password from stdin, not the terminal
 *
 * Administrators -- the members of the "wheel" group -- become root with
 * their OWN password, as on Ubuntu or macOS; root itself has none (its
 * account is locked). ary is setuid root: it checks the password against
 * /etc/shadow, then makes itself root for real and execs the command.
 *
 * Copyright (C) 2026 Red_Artem39. GPL-2.0-or-later.
 * Build: gcc -O2 -o build/ary apps/ary/ary.c apps/accounts/accounts.c -lcrypt
 */
#define _GNU_SOURCE
#include "../accounts/accounts.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static int g_stdin;   /* -S */

/* one line, without echo when it comes from the terminal */
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

int main(int argc, char **argv)
{
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-S")) { g_stdin = 1; i++; }
    if (i >= argc || !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
        printf("usage: ary [-S] CMD [ARGS...] | ary su\n"
               "Members of the '%s' group run CMD as root with their own password.\n", ACCT_ADMIN_GROUP);
        return i >= argc ? 1 : 0;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "ary: not installed setuid root\n");
        return 1;
    }
    uid_t real = getuid();
    if (real != 0) {
        struct passwd *pw = getpwuid(real);
        if (!pw) { fprintf(stderr, "ary: who are you?\n"); return 1; }
        char user[64];
        snprintf(user, sizeof user, "%s", pw->pw_name);
        if (!acct_in_group(user, ACCT_ADMIN_GROUP)) {
            fprintf(stderr, "ary: %s is not an administrator (not in the '%s' group)\n", user, ACCT_ADMIN_GROUP);
            return 1;
        }
        char prompt[96], pwd[256];
        snprintf(prompt, sizeof prompt, "[ary] password for %s: ", user);
        ask(prompt, pwd, sizeof pwd);
        bool ok = acct_check_password(user, pwd);
        explicit_bzero(pwd, sizeof pwd);
        if (!ok) { fprintf(stderr, "ary: incorrect password\n"); return 1; }
    }
    /* root for real: real, effective and saved ids, root's groups */
    if (setgroups(0, NULL) < 0 || setresgid(0, 0, 0) < 0 || setresuid(0, 0, 0) < 0) {
        fprintf(stderr, "ary: could not become root: %s\n", strerror(errno));
        return 1;
    }
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
