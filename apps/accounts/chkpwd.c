/*
 * WynlandOS - chkpwd: is this my password? (what unix_chkpwd is to PAM).
 *
 *   /usr/lib/wynland/chkpwd USER      the password on stdin, one line
 *
 * Exit 0: it is USER's password; 1: it is not (after a pause -- guessing
 * at speed is not on); 2: misuse. Nothing is printed.
 *
 * For a session's lock screen -- any shell's: xvlet's Lock.qml,
 * hyprlock-like programs -- run as the user, who cannot read /etc/shadow.
 * Setuid root for that read only: USER must be the caller's own account
 * (root may ask about anyone), so it answers nothing about other people.
 *
 * Copyright (C) 2026 Red_Artem39. GPL-2.0-or-later.
 */
#define _GNU_SOURCE
#include "accounts.h"

#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 2 || isatty(0)) {
        fprintf(stderr, "usage: chkpwd USER  (the password on stdin; for lock screens)\n");
        return 2;
    }
    uid_t real = getuid();
    struct passwd *pw = getpwuid(real);
    if (!pw) return 2;
    if (real != 0 && strcmp(pw->pw_name, argv[1])) return 2;   /* only your own */
    char user[64], pwd[256];
    snprintf(user, sizeof user, "%s", argv[1]);
    size_t n = 0;
    char ch;
    while (n + 1 < sizeof pwd && read(0, &ch, 1) == 1 && ch != '\n' && ch != 0) pwd[n++] = ch;
    pwd[n] = 0;
    /* (acct_check_password pauses on a wrong one) */
    bool ok = acct_check_password(user, pwd);
    explicit_bzero(pwd, sizeof pwd);
    return ok ? 0 : 1;
}
