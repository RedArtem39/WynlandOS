/*
 * WynlandOS - wifi: the Wi-Fi from a terminal (wifid's control socket).
 *
 *   wifi                      the link: network, signal
 *   wifi list                 scan, then the networks around
 *   wifi connect NAME [PASS]  join (the password is asked if it is not given
 *                             and not known yet); it is remembered
 *   wifi forget NAME          the password forgotten
 *   wifi off                  leave, no automatic joining until "connect"
 *   wifi raw REQUEST          wifid's own answer, for programs (the shell)
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static int ask(const char *req, char *buf, size_t cap)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    memcpy(a.sun_path, "\0wifid", 6);
    if (s < 0 || connect(s, (struct sockaddr *)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 6)) < 0) {
        fprintf(stderr, "wifi: wifid is not running\n");
        exit(1);
    }
    if (write(s, req, strlen(req)) < 0 || write(s, "\n", 1) < 0) { close(s); return -1; }
    size_t n = 0;
    for (;;) {
        ssize_t r = read(s, buf + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        buf[n] = 0;
        if (n >= 2 && (!strcmp(buf + n - 2, ".\n") && (n == 2 || buf[n - 3] == '\n'))) break;
        if (n >= cap - 1) break;
    }
    buf[n] = 0;
    close(s);
    if (n >= 2) buf[n - 2] = 0;                 /* the final "." */
    return (int)n;
}

static const char *field(const char *b, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *l = b; *l;) {
        const char *e = strchr(l, '\n');
        size_t ll = e ? (size_t)(e - l) : strlen(l);
        if (ll > kl && !strncmp(l, key, kl) && l[kl] == '\t') {
            size_t vl = ll - kl - 1 < cap - 1 ? ll - kl - 1 : cap - 1;
            memcpy(out, l + kl + 1, vl);
            out[vl] = 0;
            return out;
        }
        if (!e) break;
        l = e + 1;
    }
    out[0] = 0;
    return NULL;
}

static int bars(int rssi) { return rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : 1; }

static void show_status(void)
{
    char b[2048], v[80], ss[80], sig[16], ch[16], why[100];
    ask("status", b, sizeof b);
    field(b, "state", v, sizeof v);
    if (!strcmp(v, "connected")) {
        field(b, "ssid", ss, sizeof ss);
        field(b, "signal", sig, sizeof sig);
        field(b, "channel", ch, sizeof ch);
        int c = atoi(ch);
        printf("connected to %s  (%s dBm, channel %d, %s GHz)\n", ss, sig, c, c > 14 ? "5" : "2.4");
    } else {
        printf("%s\n", v[0] ? v : "unknown");
        if (field(b, "reason", why, sizeof why)) printf("  last: %s\n", why);
    }
}

static void show_list(void)
{
    char b[8192];
    ask("scan", b, sizeof b);
    for (int i = 0; i < 100; i++) {                 /* the scan: a few seconds */
        char st[64];
        ask("status", b, sizeof b);
        field(b, "scanning", st, sizeof st);
        if (i > 3 && strcmp(st, "1")) break;
        struct timespec t = { 0, 100000000 };
        nanosleep(&t, NULL);
    }
    ask("list", b, sizeof b);
    printf("%-4s %-32s %8s %5s  %s\n", "", "network", "signal", "ch", "security");
    for (char *l = b; *l;) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        char *f[8] = { 0 };
        int nf = 0;
        for (char *p = l; nf < 8;) { f[nf++] = p; char *t = strchr(p, '\t'); if (!t) break; *t = 0; p = t + 1; }
        if (nf >= 7 && !strcmp(f[0], "net")) {
            const char *mark = atoi(f[6]) ? " *  " : atoi(f[5]) == 1 ? " +  " : atoi(f[5]) == 2 ? " !  " : "    ";
            static const char *b4[5] = { "", "#   ", "##  ", "### ", "####" };
            printf("%s %-32s %4s %s %5s  %s\n", mark, f[1], f[2], b4[bars(atoi(f[2]))], f[3], f[4]);
        }
        if (!e) break;
        l = e + 1;
    }
    printf("\n  * connected   + password known   ! the known password was refused\n");
}

static void read_secret(const char *prompt, char *out, size_t cap)
{
    struct termios o, n;
    bool tty = tcgetattr(0, &o) == 0;
    fputs(prompt, stderr);
    if (tty) { n = o; n.c_lflag &= ~(tcflag_t)ECHO; tcsetattr(0, TCSANOW, &n); }
    if (!fgets(out, (int)cap, stdin)) out[0] = 0;
    if (tty) { tcsetattr(0, TCSANOW, &o); fputc('\n', stderr); }
    out[strcspn(out, "\r\n")] = 0;
}

int main(int argc, char **argv)
{
    char b[8192];
    if (argc < 2 || !strcmp(argv[1], "status")) { show_status(); return 0; }
    if (!strcmp(argv[1], "list") || !strcmp(argv[1], "scan")) { show_list(); return 0; }
    if (!strcmp(argv[1], "raw") && argc >= 3) {
        char req[256];
        if (!strcmp(argv[2], "-")) {                    /* the request on stdin: a password stays out of argv */
            if (!fgets(req, sizeof req, stdin)) return 1;
            req[strcspn(req, "\r\n")] = 0;
        } else snprintf(req, sizeof req, "%s", argv[2]);
        ask(req, b, sizeof b);
        memset(req, 0, sizeof req);
        fputs(b, stdout);
        return 0;
    }
    if (!strcmp(argv[1], "off") || !strcmp(argv[1], "disconnect")) { ask("disconnect", b, sizeof b); return 0; }
    if (!strcmp(argv[1], "forget") && argc >= 3) { snprintf(b, sizeof b, "forget %s", argv[2]); ask(b, b, sizeof b); return 0; }
    if (!strcmp(argv[1], "connect") && argc >= 3) {
        char pw[100] = "", req[256], v[120];
        if (argc >= 4) snprintf(pw, sizeof pw, "%s", argv[3]);
        snprintf(req, sizeof req, "connect %s%s%s", argv[2], pw[0] ? "\t" : "", pw);
        ask(req, b, sizeof b);
        if (field(b, "error", v, sizeof v) && !strcmp(v, "need a password")) {
            read_secret("password: ", pw, sizeof pw);
            snprintf(req, sizeof req, "connect %s\t%s", argv[2], pw);
            ask(req, b, sizeof b);
        }
        memset(pw, 0, sizeof pw);
        memset(req, 0, sizeof req);
        if (field(b, "error", v, sizeof v)) { fprintf(stderr, "wifi: %s\n", v); return 1; }
        printf("connecting to %s ...\n", argv[2]);
        for (int i = 0; i < 300; i++) {                 /* up to 30 s */
            char st[64], why[100];
            struct timespec t = { 0, 100000000 };
            nanosleep(&t, NULL);
            ask("status", b, sizeof b);
            field(b, "state", st, sizeof st);
            if (!strcmp(st, "connected")) { show_status(); return 0; }
            if (!strcmp(st, "idle") && i > 10) {
                fprintf(stderr, "wifi: failed: %s\n", field(b, "reason", why, sizeof why) ? why : "unknown");
                return 1;
            }
        }
        fprintf(stderr, "wifi: still connecting\n");
        return 1;
    }
    fprintf(stderr, "usage: wifi [status | list | connect NAME [PASSWORD] | forget NAME | off]\n");
    return 2;
}
