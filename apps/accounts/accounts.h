/*
 * WynlandOS - user accounts: /etc/passwd, /etc/group, /etc/shadow.
 *
 * The standard files and formats, so glibc's getpwnam() & co. (ls -l,
 * id, whoami, fish's prompt) know the names. Passwords are yescrypt
 * hashes (libxcrypt, as Ubuntu). Every user gets a group of its own name
 * and id; administrators are the members of "wheel", who may become root
 * with their own password (ary).
 *
 * Used by ary, useradd, userdel, passwd and the login daemon (wynlogin).
 * The changing calls need root.
 *
 * Copyright (C) 2026 Red_Artem39. GPL-2.0-only.
 */
#pragma once
#include <stdbool.h>
#include <sys/types.h>

#define ACCT_FIRST_UID 1000
#define ACCT_ADMIN_GROUP "wheel"

/* a valid user name: a letter or _, then letters, digits, _ . - (<= 31) */
bool acct_valid_name(const char *name);
/* does the password match the user's? (false also for locked accounts) */
bool acct_check_password(const char *user, const char *password);
/* set it (yescrypt); 0 or -errno */
int acct_set_password(const char *user, const char *password);
/* a new user: uid/gid from 1000 up, a group of its own, /home/NAME from
   /etc/skel, in wheel when admin; locked until a password is set.
   0 or -errno (-EEXIST: the name is taken) */
int acct_add_user(const char *name, const char *full_name, bool admin);
/* remove the user (and its group; its home too when remove_home) */
int acct_del_user(const char *name, bool remove_home);
/* is the user in the group (as its primary group or a member)? */
bool acct_in_group(const char *user, const char *group);
/* how many accounts with uid >= 1000 there are */
int acct_count_users(void);
