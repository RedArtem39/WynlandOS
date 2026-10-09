/*
 * WynlandOS - user accounts (accounts.h).
 *
 * Copyright (C) 2026 Red_Artem39. GPL-2.0-only.
 */
#define _GNU_SOURCE
#include "accounts.h"

#include <crypt.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PASSWD "/etc/passwd"
#define GROUP  "/etc/group"
#define SHADOW "/etc/shadow"

bool acct_valid_name(const char *n)
{
    if (!n || !*n || strlen(n) > 31) return false;
    if (!isalpha((unsigned char)n[0]) && n[0] != '_') return false;
    for (const char *p = n; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '.' && *p != '-') return false;
    return true;
}

/* the whole file, or NULL */
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    size_t got;
    while (buf && (got = fread(buf + n, 1, cap - n - 1, f)) > 0) {
        n += got;
        if (cap - n < 2) { cap *= 2; char *nb = realloc(buf, cap); if (!nb) { free(buf); buf = NULL; } else buf = nb; }
    }
    fclose(f);
    if (buf) buf[n] = 0;
    return buf;
}

/* replace a file whole: write a sibling, then rename over it */
static int replace_file(const char *path, const char *content, mode_t mode)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -errno;
    size_t len = strlen(content), off = 0;
    while (off < len) {
        ssize_t w = write(fd, content + off, len - off);
        if (w <= 0) { int e = errno; close(fd); unlink(tmp); return -(e ? e : EIO); }
        off += (size_t)w;
    }
    fchmod(fd, mode);
    fsync(fd);
    close(fd);
    if (rename(tmp, path) < 0) { int e = errno; unlink(tmp); return -e; }
    return 0;
}

/* the line of a ':'-file whose first field is name: start offset, or -1 */
static long find_line(const char *buf, const char *name)
{
    size_t nl = strlen(name);
    for (const char *p = buf; p && *p; ) {
        if (!strncmp(p, name, nl) && p[nl] == ':') return p - buf;
        p = strchr(p, '\n');
        if (p) p++;
    }
    return -1;
}

/* buf with the line starting at off replaced by repl (NULL: removed) */
static char *splice_line(const char *buf, long off, const char *repl)
{
    const char *end = strchr(buf + off, '\n');
    end = end ? end + 1 : buf + strlen(buf);
    size_t rl = repl ? strlen(repl) : 0;
    char *out = malloc(strlen(buf) + rl + 2);
    if (!out) return NULL;
    memcpy(out, buf, (size_t)off);
    size_t o = (size_t)off;
    if (repl) { memcpy(out + o, repl, rl); o += rl; out[o++] = '\n'; }
    strcpy(out + o, end);
    return out;
}

static char *append_line(const char *buf, const char *line)
{
    size_t bl = buf ? strlen(buf) : 0;
    char *out = malloc(bl + strlen(line) + 3);
    if (!out) return NULL;
    memcpy(out, buf ? buf : "", bl);
    size_t o = bl;
    if (o && out[o - 1] != '\n') out[o++] = '\n';
    strcpy(out + o, line);
    strcat(out, "\n");
    return out;
}

/* the hash field of the user's shadow line, malloc'd, or NULL */
static char *shadow_hash(const char *user)
{
    char *buf = slurp(SHADOW);
    if (!buf) return NULL;
    long off = find_line(buf, user);
    char *hash = NULL;
    if (off >= 0) {
        const char *h = buf + off + strlen(user) + 1;
        const char *e = h;
        while (*e && *e != ':' && *e != '\n') e++;
        hash = strndup(h, (size_t)(e - h));
    }
    explicit_bzero(buf, strlen(buf));
    free(buf);
    return hash;
}

bool acct_check_password(const char *user, const char *password)
{
    char *hash = shadow_hash(user);
    if (!hash) return false;
    bool ok = false;
    if (hash[0] == '$') {                    /* "!", "*", "": locked or no password */
        struct crypt_data cd;
        memset(&cd, 0, sizeof cd);
        const char *r = crypt_r(password, hash, &cd);
        if (r && r[0] != '*') {
            size_t a = strlen(r), b = strlen(hash);
            unsigned diff = (unsigned)(a ^ b);
            for (size_t i = 0; i < a && i < b; i++) diff |= (unsigned)(r[i] ^ hash[i]);
            ok = diff == 0;
        }
        explicit_bzero(&cd, sizeof cd);
    }
    free(hash);
    if (!ok) sleep(2);                       /* a wrong guess costs time: no brute force */
    return ok;
}

int acct_set_password(const char *user, const char *password)
{
    if (!password || !*password) return -EINVAL;
    char salt[CRYPT_GENSALT_OUTPUT_SIZE];
    if (!crypt_gensalt_rn("$y$", 0, NULL, 0, salt, sizeof salt)) return -errno;
    struct crypt_data cd;
    memset(&cd, 0, sizeof cd);
    const char *h = crypt_r(password, salt, &cd);
    if (!h || h[0] == '*') return -EINVAL;
    char *buf = slurp(SHADOW);
    if (!buf) buf = strdup("");
    char line[512];
    long days = (long)(time(NULL) / 86400);
    snprintf(line, sizeof line, "%s:%s:%ld:0:99999:7:::", user, h, days);
    explicit_bzero(&cd, sizeof cd);
    long off = find_line(buf, user);
    char *out = off >= 0 ? splice_line(buf, off, line) : append_line(buf, line);
    explicit_bzero(line, sizeof line);
    explicit_bzero(buf, strlen(buf));
    free(buf);
    if (!out) return -ENOMEM;
    int r = replace_file(SHADOW, out, 0600);
    explicit_bzero(out, strlen(out));
    free(out);
    return r;
}

static int add_line_to(const char *path, const char *line, mode_t mode)
{
    char *buf = slurp(path);
    char *out = append_line(buf, line);
    free(buf);
    if (!out) return -ENOMEM;
    int r = replace_file(path, out, mode);
    free(out);
    return r;
}

static int remove_line_from(const char *path, const char *name, mode_t mode)
{
    char *buf = slurp(path);
    if (!buf) return -errno;
    long off = find_line(buf, name);
    if (off < 0) { free(buf); return 0; }
    char *out = splice_line(buf, off, NULL);
    free(buf);
    if (!out) return -ENOMEM;
    int r = replace_file(path, out, mode);
    free(out);
    return r;
}

/* add or drop user in the member list of group */
static int set_group_member(const char *group, const char *user, bool add)
{
    char *buf = slurp(GROUP);
    if (!buf) return -errno;
    long off = find_line(buf, group);
    if (off < 0) { free(buf); return -ENOENT; }
    const char *s = buf + off, *e = strchr(s, '\n');
    char line[1024];
    size_t ll = e ? (size_t)(e - s) : strlen(s);
    if (ll >= sizeof line - 40) { free(buf); return -E2BIG; }
    memcpy(line, s, ll);
    line[ll] = 0;
    /* name:x:gid:members */
    char *members = strrchr(line, ':');
    if (!members) { free(buf); return -EINVAL; }
    members++;
    char out[1024] = "";
    size_t prefix = (size_t)(members - line);
    memcpy(out, line, prefix);
    out[prefix] = 0;
    bool found = false, first = true;
    char *save = NULL;
    for (char *m = strtok_r(members, ",", &save); m; m = strtok_r(NULL, ",", &save)) {
        if (!strcmp(m, user)) { found = true; if (!add) continue; }
        if (!first) strcat(out, ",");
        strcat(out, m);
        first = false;
    }
    if (add && !found) { if (!first) strcat(out, ","); strcat(out, user); }
    char *nb = splice_line(buf, off, out);
    free(buf);
    if (!nb) return -ENOMEM;
    int r = replace_file(GROUP, nb, 0644);
    free(nb);
    return r;
}

bool acct_in_group(const char *user, const char *group)
{
    struct group *g = getgrnam(group);
    struct passwd *pw = getpwnam(user);
    if (!g) return false;
    if (pw && pw->pw_gid == g->gr_gid) return true;
    for (char **m = g->gr_mem; m && *m; m++)
        if (!strcmp(*m, user)) return true;
    return false;
}

int acct_count_users(void)
{
    int n = 0;
    setpwent();
    for (struct passwd *p; (p = getpwent()); )
        if (p->pw_uid >= ACCT_FIRST_UID && p->pw_uid < 60000) n++;
    endpwent();
    return n;
}

/* copy /etc/skel into a new home, owned by the user */
static void copy_skel(const char *from, const char *to, uid_t uid, gid_t gid)
{
    DIR *d = opendir(from);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d)); ) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s[512], t[512];
        snprintf(s, sizeof s, "%s/%s", from, e->d_name);
        snprintf(t, sizeof t, "%s/%s", to, e->d_name);
        struct stat st;
        if (lstat(s, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) {
            mkdir(t, st.st_mode & 0777);
            chown(t, uid, gid);
            copy_skel(s, t, uid, gid);
        } else if (S_ISREG(st.st_mode)) {
            int in = open(s, O_RDONLY), out = open(t, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
            char buf[8192];
            ssize_t n;
            while (in >= 0 && out >= 0 && (n = read(in, buf, sizeof buf)) > 0) write(out, buf, (size_t)n);
            if (in >= 0) close(in);
            if (out >= 0) { fchown(out, uid, gid); close(out); }
        }
    }
    closedir(d);
}

int acct_add_user(const char *name, const char *full_name, bool admin)
{
    if (!acct_valid_name(name)) return -EINVAL;
    if (getpwnam(name) || getgrnam(name)) return -EEXIST;
    /* the next free id, the same for the user and its group */
    unsigned id = ACCT_FIRST_UID;
    for (;; id++) {
        if (!getpwuid(id) && !getgrgid(id)) break;
        if (id > 59999) return -ENOSPC;
    }
    char gecos[96] = "";
    if (full_name)
        for (size_t i = 0, o = 0; full_name[i] && o < sizeof gecos - 1; i++)
            if (full_name[i] != ':' && full_name[i] != '\n' && full_name[i] != ',') gecos[o++] = full_name[i];
    char line[512];
    int r;
    snprintf(line, sizeof line, "%s:x:%u:", name, id);
    if ((r = add_line_to(GROUP, line, 0644)) < 0) return r;
    snprintf(line, sizeof line, "%s:x:%u:%u:%s:/home/%s:/usr/bin/fish", name, id, id, gecos, name);
    if ((r = add_line_to(PASSWD, line, 0644)) < 0) return r;
    snprintf(line, sizeof line, "%s:!:%ld:0:99999:7:::", name, (long)(time(NULL) / 86400));
    if ((r = add_line_to(SHADOW, line, 0600)) < 0) return r;
    if (admin && (r = set_group_member(ACCT_ADMIN_GROUP, name, true)) < 0) return r;
    char home[64];
    snprintf(home, sizeof home, "/home/%s", name);
    mkdir("/home", 0755);
    if (mkdir(home, 0700) < 0 && errno != EEXIST) return -errno;
    chown(home, id, id);
    copy_skel("/etc/skel", home, id, id);
    return 0;
}

static void remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            for (struct dirent *e; (e = readdir(d)); ) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char sub[512];
                snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
                remove_tree(sub);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

int acct_del_user(const char *name, bool remove_home)
{
    struct passwd *pw = getpwnam(name);
    if (!pw || pw->pw_uid < ACCT_FIRST_UID) return -ENOENT;   /* never system accounts */
    char home[256];
    snprintf(home, sizeof home, "%s", pw->pw_dir);
    int r;
    if ((r = remove_line_from(PASSWD, name, 0644)) < 0) return r;
    if ((r = remove_line_from(SHADOW, name, 0600)) < 0) return r;
    remove_line_from(GROUP, name, 0644);
    set_group_member(ACCT_ADMIN_GROUP, name, false);
    if (remove_home && !strncmp(home, "/home/", 6)) remove_tree(home);
    return 0;
}
