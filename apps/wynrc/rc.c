// WynlandOS - rc-status / rc-service / rc-update (one binary, picked by
// the name it runs under). The first two talk to the running wynrc over
// its control socket; rc-update edits the runlevel files directly.
//
//   rc-status                       every service: state, pid, why
//   rc-service NAME start|stop|restart|status
//   rc-update add NAME [runlevel]   (default runlevel: default)
//   rc-update del NAME [runlevel]
//   rc-update show

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <unistd.h>

#include "wynrc.h"

static int ask(const char *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path, WYNRC_SOCKET, WYNRC_SOCKET_LEN);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + WYNRC_SOCKET_LEN)) < 0) {
        fprintf(stderr, "wynrc is not running\n");
        return 1;
    }
    if (write(fd, req, strlen(req)) < 0) return 1;
    shutdown(fd, SHUT_WR);
    char buf[1024];
    ssize_t n;
    int bad = 0;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        if (!strncmp(buf, "error", 5)) bad = 1;
        fwrite(buf, 1, (size_t)n, stdout);
    }
    close(fd);
    return bad;
}

static int status(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path, WYNRC_SOCKET, WYNRC_SOCKET_LEN);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + WYNRC_SOCKET_LEN)) < 0) {
        fprintf(stderr, "wynrc is not running\n");
        return 1;
    }
    if (write(fd, "status", 6) < 0) return 1;
    shutdown(fd, SHUT_WR);
    char buf[4096];
    size_t len = 0;
    ssize_t n;
    while (len < sizeof buf - 1 && (n = read(fd, buf + len, sizeof buf - 1 - len)) > 0) len += (size_t)n;
    buf[len] = 0;
    close(fd);
    printf("%-12s %-9s %6s  %s\n", "SERVICE", "STATE", "PID", "");
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char *f[4] = { line, "", "", "" };
        for (int i = 1; i < 4; i++) { char *t = strchr(f[i - 1], '\t'); if (!t) break; *t = 0; f[i] = t + 1; }
        printf("%-12s %-9s %6s  %s\n", f[0], f[1], strcmp(f[2], "0") ? f[2] : "-", f[3]);
    }
    return 0;
}

static int update(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "show")) {
        const char *levels[] = { "boot", "default" };
        for (int l = 0; l < 2; l++) {
            char path[128], line[64];
            snprintf(path, sizeof path, "%s/%s", WYNRC_RUNLEVELS, levels[l]);
            FILE *f = fopen(path, "r");
            printf("%s:", levels[l]);
            while (f && fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (line[0] && line[0] != '#') printf(" %s", line);
            }
            printf("\n");
            if (f) fclose(f);
        }
        return 0;
    }
    if (argc < 3 || (strcmp(argv[1], "add") && strcmp(argv[1], "del"))) {
        fprintf(stderr, "usage: rc-update add|del NAME [runlevel] | rc-update show\n");
        return 2;
    }
    const char *name = argv[2], *level = argc > 3 ? argv[3] : "default";
    char path[128], svc[160];
    snprintf(path, sizeof path, "%s/%s", WYNRC_RUNLEVELS, level);
    snprintf(svc, sizeof svc, "%s/%s", WYNRC_SERVICES, name);
    if (!strcmp(argv[1], "add") && access(svc, R_OK) != 0) {
        fprintf(stderr, "rc-update: no service %s (%s)\n", name, svc);
        return 1;
    }
    // rewrite the runlevel file without NAME, then append it for "add"
    char keep[64][64];
    int n = 0, had = 0;
    FILE *f = fopen(path, "r");
    char line[64];
    while (f && fgets(line, sizeof line, f) && n < 64) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, name)) { had = 1; continue; }
        snprintf(keep[n++], 64, "%s", line);
    }
    if (f) fclose(f);
    f = fopen(path, "w");
    if (!f) { perror(path); return 1; }
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", keep[i]);
    if (!strcmp(argv[1], "add")) fprintf(f, "%s\n", name);
    fclose(f);
    printf("%s %s %s runlevel %s\n", name, !strcmp(argv[1], "add") ? (had ? "already in" : "added to") : (had ? "removed from" : "was not in"),
           "the", level);
    return 0;
}

int main(int argc, char **argv)
{
    const char *me = strrchr(argv[0], '/');
    me = me ? me + 1 : argv[0];
    if (!strcmp(me, "rc-status")) return status();
    if (!strcmp(me, "rc-update")) return update(argc, argv);
    if (!strcmp(me, "rc-service")) {
        if (argc < 3) { fprintf(stderr, "usage: rc-service NAME start|stop|restart|status\n"); return 2; }
        if (!strcmp(argv[2], "status")) return status();
        char req[96];
        snprintf(req, sizeof req, "%s %s", argv[2], argv[1]);
        return ask(req);
    }
    fprintf(stderr, "run as rc-status, rc-service or rc-update\n");
    return 2;
}
