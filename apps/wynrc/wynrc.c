// WynlandOS - wynrc: the init (PID-1 role, /sbin/init).
//
// OpenRC's model, without a shell: services are declarative files in
// /etc/wynrc/services (key=value), runlevels list them
// (/etc/wynrc/runlevels/{boot,default}, one name per line). Everything
// whose dependencies are met starts at once (in parallel); daemons are
// supervised and restarted by policy; oneshots run to completion.
//
// Service file keys:
//   description=  free text
//   type=         daemon (long-running) | oneshot (runs to completion)
//   exec=         program and arguments (split on spaces)
//   need=         services/virtuals that must be up first; failure fails this
//   use=          started first if they are in the runlevel; not required
//   after=        ordering only: wait for them if they are being started
//   provide=      a virtual name (e.g. display); the first provider in
//                 runlevel order wins, the other providers are skipped
//   when=         facts that must hold, e.g. "zerp=2 virgl=1": from
//                 /etc/wynland/boot.cfg (make's choices) and
//                 /etc/wynland/hw (what the kernel found)
//   restart=      always | on-failure | no      (daemons; default on-failure)
//
// Control: an AF_UNIX socket (abstract "wynrc"), one line per request:
// "status", "start NAME", "stop NAME", "restart NAME".

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "wynrc.h"

#define MAX_SVC   64
#define MAX_LIST  16
#define MAX_FACTS 64

enum State { S_WAITING, S_STARTING, S_RUNNING, S_DONE, S_FAILED, S_SKIPPED, S_STOPPED };
static const char *state_name[] = { "waiting", "starting", "running", "done", "failed", "skipped", "stopped" };

typedef struct {
    char name[32];
    char description[96];
    char exec[256];
    int oneshot;
    char need[MAX_LIST][32];  int n_need;
    char use[MAX_LIST][32];   int n_use;
    char after[MAX_LIST][32]; int n_after;
    char provide[32];
    char when[256];
    int restart;              /* 0 no, 1 on-failure, 2 always */
    int state;
    char info[96];
    pid_t pid;
    long started_ms, ended_ms;
    int restarts;
    long restart_at_ms;       /* pending restart time, 0 = none */
    long restart_window_ms;
    int stop_requested;
} Svc;

static Svc g_svc[MAX_SVC];
static int g_n;
static char g_fact_key[MAX_FACTS][32], g_fact_val[MAX_FACTS][32];
static int g_nfacts;
static long g_t0;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void logf_(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[wynrc +%ld ms] %s\n", now_ms(), buf);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    return s;
}

static int split_words(const char *src, char out[][32], int max)
{
    int n = 0;
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", src);
    for (char *t = strtok(tmp, " ,\t"); t && n < max; t = strtok(NULL, " ,\t"))
        snprintf(out[n++], 32, "%s", t);
    return n;
}

// ---------------------------------------------------------------- facts

static void load_facts(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f) && g_nfacts < MAX_FACTS) {
        char *l = trim(line);
        if (!*l || *l == '#') continue;
        char *eq = strchr(l, '=');
        if (eq) { *eq = 0; snprintf(g_fact_key[g_nfacts], 32, "%s", trim(l)); snprintf(g_fact_val[g_nfacts], 32, "%s", trim(eq + 1)); }
        else    { snprintf(g_fact_key[g_nfacts], 32, "%s", l); snprintf(g_fact_val[g_nfacts], 32, "1"); }
        g_nfacts++;
    }
    fclose(f);
}

static const char *fact(const char *key)
{
    for (int i = g_nfacts - 1; i >= 0; i--)   /* later files override */
        if (!strcmp(g_fact_key[i], key)) return g_fact_val[i];
    return NULL;
}

// "k=v k2=v2": all must hold; "k=a|b" takes either. A missing fact compares as "0".
static int conditions_hold(const char *when, char *why, size_t whylen)
{
    char terms[MAX_LIST][32];
    int n = split_words(when, terms, MAX_LIST);
    for (int i = 0; i < n; i++) {
        char k[32], *eq;
        snprintf(k, sizeof k, "%s", terms[i]);
        eq = strchr(k, '=');
        const char *want = eq ? eq + 1 : "1";
        if (eq) *eq = 0;
        const char *have = fact(k);
        if (!have) have = "0";
        // "k=a|b": either value
        int ok = 0;
        for (const char *w = want; *w && !ok; ) {
            size_t len = strcspn(w, "|");
            ok = strlen(have) == len && !strncmp(have, w, len);
            w += len + (w[len] == '|');
        }
        if (!ok) { snprintf(why, whylen, "%s=%s (want %s)", k, have, want); return 0; }
    }
    return 1;
}

// ---------------------------------------------------------------- services

static Svc *find(const char *name)
{
    for (int i = 0; i < g_n; i++) if (!strcmp(g_svc[i].name, name)) return &g_svc[i];
    return NULL;
}

// a service, or the active provider of a virtual
static Svc *resolve(const char *name)
{
    Svc *s = find(name);
    if (s) return s;
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_svc[i].provide, name) && g_svc[i].state != S_SKIPPED) return &g_svc[i];
    return NULL;
}

static int load_service(const char *name)
{
    if (find(name) || g_n >= MAX_SVC) return 0;
    char path[160];
    snprintf(path, sizeof path, "%s/%s", WYNRC_SERVICES, name);
    FILE *f = fopen(path, "r");
    if (!f) { logf_("no such service: %s", name); return -1; }
    Svc *s = &g_svc[g_n++];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->restart = 1;
    char line[320];
    while (fgets(line, sizeof line, f)) {
        char *l = trim(line);
        if (!*l || *l == '#') continue;
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(l), *v = trim(eq + 1);
        if (!strcmp(k, "description")) snprintf(s->description, sizeof s->description, "%s", v);
        else if (!strcmp(k, "exec")) snprintf(s->exec, sizeof s->exec, "%s", v);
        else if (!strcmp(k, "type")) s->oneshot = !strcmp(v, "oneshot");
        else if (!strcmp(k, "need")) s->n_need = split_words(v, s->need, MAX_LIST);
        else if (!strcmp(k, "use")) s->n_use = split_words(v, s->use, MAX_LIST);
        else if (!strcmp(k, "after")) s->n_after = split_words(v, s->after, MAX_LIST);
        else if (!strcmp(k, "provide")) snprintf(s->provide, sizeof s->provide, "%s", v);
        else if (!strcmp(k, "when")) snprintf(s->when, sizeof s->when, "%s", v);
        else if (!strcmp(k, "restart")) s->restart = !strcmp(v, "always") ? 2 : !strcmp(v, "no") ? 0 : 1;
    }
    fclose(f);
    return 0;
}

static void load_runlevel(const char *level)
{
    char path[160];
    snprintf(path, sizeof path, "%s/%s", WYNRC_RUNLEVELS, level);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];   /* whole lines: a long comment must not split into a "name" */
    while (fgets(line, sizeof line, f)) {
        char *l = trim(line);
        if (*l && *l != '#') load_service(l);
    }
    fclose(f);
}

// conditions and virtuals, once, before anything starts
static void plan(void)
{
    for (int i = 0; i < g_n; i++) {
        Svc *s = &g_svc[i];
        char why[96];
        if (s->when[0] && !conditions_hold(s->when, why, sizeof why)) {
            s->state = S_SKIPPED;
            snprintf(s->info, sizeof s->info, "condition %s", why);
            continue;
        }
        if (s->provide[0]) {
            for (int j = 0; j < i; j++) {
                Svc *o = &g_svc[j];
                if (o->state != S_SKIPPED && !strcmp(o->provide, s->provide)) {
                    s->state = S_SKIPPED;
                    snprintf(s->info, sizeof s->info, "%s provided by %s", s->provide, o->name);
                    break;
                }
            }
        }
    }
}

static int is_ready(const Svc *d)
{
    return d->state == S_RUNNING || d->state == S_DONE;
}
static int is_settled(const Svc *d)
{
    return d->state != S_WAITING && d->state != S_STARTING;
}

// 1 = can start now, 0 = not yet, -1 = never (a need failed)
static int deps_state(Svc *s)
{
    for (int i = 0; i < s->n_need; i++) {
        Svc *d = resolve(s->need[i]);
        if (!d || d->state == S_SKIPPED || d->state == S_FAILED || d->state == S_STOPPED) {
            snprintf(s->info, sizeof s->info, "needs %s, which is %s", s->need[i],
                     d ? state_name[d->state] : "missing");
            return -1;
        }
        if (!is_ready(d)) return 0;
    }
    for (int i = 0; i < s->n_use; i++) {
        Svc *d = resolve(s->use[i]);
        if (d && d->state != S_SKIPPED && !is_ready(d) && !is_settled(d)) return 0;
    }
    for (int i = 0; i < s->n_after; i++) {
        Svc *d = resolve(s->after[i]);
        if (d && d->state != S_SKIPPED && !is_settled(d)) return 0;
        if (d && !d->oneshot && d->state == S_STARTING) return 0;
    }
    return 1;
}

static void start(Svc *s)
{
    char buf[256], *argv[24];
    int argc = 0;
    snprintf(buf, sizeof buf, "%s", s->exec);
    for (char *t = strtok(buf, " "); t && argc < 23; t = strtok(NULL, " ")) argv[argc++] = t;
    argv[argc] = NULL;
    if (!argc) { s->state = S_FAILED; snprintf(s->info, sizeof s->info, "no exec="); return; }

    /* The kernel's own process creation (SYS_spawn_argv, what the kernel
       and Zerp use): a fresh image, this process as its parent. A
       fork()+execv() here sometimes left zerp2's dynamic loader crashing
       in its first dlopen(); fork+exec stays the fallback. */
    pid_t pid = (pid_t)syscall(408, argv[0], argv, 0);
    if (pid <= 0) {
        pid = fork();
        if (pid == 0) {
            setsid();
            execv(argv[0], argv);
            fprintf(stderr, "[wynrc] exec %s: %s\n", argv[0], strerror(errno));
            _exit(127);
        }
    }
    if (pid < 0) { s->state = S_FAILED; snprintf(s->info, sizeof s->info, "fork: %s", strerror(errno)); return; }
    s->pid = pid;
    s->started_ms = now_ms();
    s->state = s->oneshot ? S_STARTING : S_RUNNING;
    s->info[0] = 0;
    s->stop_requested = 0;
    logf_("started %s (pid %d)%s%s", s->name, (int)pid, s->description[0] ? ": " : "", s->description);
}

static void on_exit_(Svc *s, int status)
{
    s->ended_ms = now_ms();
    s->pid = 0;
    const int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    char how[48];
    if (WIFEXITED(status)) snprintf(how, sizeof how, "exit %d", WEXITSTATUS(status));
    else snprintf(how, sizeof how, "signal %d", WIFSIGNALED(status) ? WTERMSIG(status) : 0);

    if (s->oneshot) {
        s->state = ok ? S_DONE : S_FAILED;
        snprintf(s->info, sizeof s->info, "%s after %ld ms", how, s->ended_ms - s->started_ms);
        logf_("%s %s (%s)", s->name, ok ? "done" : "FAILED", s->info);
        return;
    }
    if (s->stop_requested) {
        s->state = S_STOPPED;
        snprintf(s->info, sizeof s->info, "stopped (%s)", how);
        logf_("%s stopped", s->name);
        return;
    }
    const int again = s->restart == 2 || (s->restart == 1 && !ok);
    // crash loop guard: at most 5 restarts within 30 s
    if (s->ended_ms - s->restart_window_ms > 30000) { s->restart_window_ms = s->ended_ms; s->restarts = 0; }
    if (again && s->restarts < 5) {
        s->restarts++;
        s->restart_at_ms = s->ended_ms + 500L * s->restarts;
        s->state = S_WAITING;
        snprintf(s->info, sizeof s->info, "%s, restart #%d", how, s->restarts);
        logf_("%s exited (%s), restarting", s->name, how);
    } else {
        s->state = S_FAILED;
        snprintf(s->info, sizeof s->info, "%s%s", how, again ? ", too many restarts" : "");
        logf_("%s exited (%s)", s->name, s->info);
    }
}

static void reap(void)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < g_n; i++)
            if (g_svc[i].pid == pid) { on_exit_(&g_svc[i], status); break; }
    }
}

static int schedule(void)
{
    int progress = 0;
    const long now = now_ms();
    for (int i = 0; i < g_n; i++) {
        Svc *s = &g_svc[i];
        if (s->state != S_WAITING) continue;
        if (s->restart_at_ms && now < s->restart_at_ms) continue;
        const int d = deps_state(s);
        if (d < 0) { s->state = S_FAILED; logf_("%s not started: %s", s->name, s->info); progress = 1; continue; }
        if (d == 0) continue;
        s->restart_at_ms = 0;
        start(s);
        progress = 1;
    }
    return progress;
}

// ---------------------------------------------------------------- control

static void reply_status(int fd)
{
    char line[256];
    for (int i = 0; i < g_n; i++) {
        Svc *s = &g_svc[i];
        int n = snprintf(line, sizeof line, "%s\t%s\t%d\t%s\n", s->name, state_name[s->state], (int)s->pid, s->info);
        if (write(fd, line, (size_t)n) < 0) return;
    }
}

static void handle_request(int fd, char *req)
{
    char *cmd = strtok(trim(req), " ");
    char *arg = strtok(NULL, " ");
    char out[160];
    if (!cmd) return;
    if (!strcmp(cmd, "status")) { reply_status(fd); return; }
    Svc *s = arg ? find(arg) : NULL;
    if (!s && arg && (!strcmp(cmd, "start") || !strcmp(cmd, "restart"))) {
        if (load_service(arg) == 0) s = find(arg);
    }
    if (!s) { snprintf(out, sizeof out, "error: no service %s\n", arg ? arg : "(none)"); }
    else if (!strcmp(cmd, "stop") || !strcmp(cmd, "restart")) {
        if (s->pid > 0) { s->stop_requested = 1; kill(s->pid, SIGTERM); }
        if (!strcmp(cmd, "restart")) { s->state = S_WAITING; s->restarts = 0; s->restart_at_ms = now_ms() + 300; }
        else if (!s->pid) s->state = S_STOPPED;
        snprintf(out, sizeof out, "ok: %s %s\n", cmd, s->name);
    } else if (!strcmp(cmd, "start")) {
        if (s->state == S_RUNNING || s->state == S_STARTING) snprintf(out, sizeof out, "ok: %s already %s\n", s->name, state_name[s->state]);
        else { s->state = S_WAITING; s->restarts = 0; s->restart_at_ms = 0; s->info[0] = 0; snprintf(out, sizeof out, "ok: start %s\n", s->name); }
    } else snprintf(out, sizeof out, "error: unknown command %s\n", cmd);
    if (write(fd, out, strlen(out)) < 0) { /* client gone */ }
}

int main(void)
{
    g_t0 = now_ms();
    if (!getenv("PATH")) setenv("PATH", "/usr/bin:/bin:/sbin", 1);
    if (!getenv("HOME")) setenv("HOME", "/tmp", 1);
    signal(SIGPIPE, SIG_IGN);

    load_facts(WYNRC_BOOTCFG);
    load_facts(WYNRC_HWFACTS);
    load_runlevel("boot");
    load_runlevel("default");
    plan();
    logf_("%d services, %d facts", g_n, g_nfacts);
    for (int i = 0; i < g_n; i++)
        if (g_svc[i].state == S_SKIPPED) logf_("skip %s: %s", g_svc[i].name, g_svc[i].info);

    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path, WYNRC_SOCKET, WYNRC_SOCKET_LEN);
    if (ls >= 0 && (bind(ls, (struct sockaddr *)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + WYNRC_SOCKET_LEN)) < 0 ||
                    listen(ls, 8) < 0)) {
        logf_("control socket: %s", strerror(errno));
        close(ls);
        ls = -1;
    }

    int all_settled_logged = 0;
    for (;;) {
        reap();
        while (schedule()) reap();

        if (!all_settled_logged) {
            int settled = 1;
            for (int i = 0; i < g_n; i++) if (g_svc[i].state == S_WAITING || g_svc[i].state == S_STARTING) settled = 0;
            if (settled) { logf_("runlevel default reached (%ld ms after init started)", now_ms() - g_t0); all_settled_logged = 1; }
        }

        struct pollfd p = { ls, POLLIN, 0 };
        if (poll(&p, ls >= 0 ? 1 : 0, 100) > 0 && (p.revents & POLLIN)) {
            int c = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0) {
                char req[128];
                ssize_t n = read(c, req, sizeof req - 1);
                if (n > 0) { req[n] = 0; handle_request(c, req); }
                close(c);
            }
        }
    }
}
