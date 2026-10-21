/*
 * WynlandOS - /proc's per-process files, as Linux has them: what ps, top,
 * pgrep/pkill and desktop shells read.
 *
 *   /proc                    the processes' PIDs among the system files
 *   /proc/PID (self, thread-self)
 *        stat, statm, status, comm, cmdline
 *
 * PIDs are the reader's (its pid namespace: kernel/sandbox.c); a process it
 * cannot see does not exist for it. The files are made when opened
 * (kernel/syscall.c hands them out as memfds), so they are a snapshot.
 */
#include <wynland/process.h>
#include <wynland/sched.h>
#include <wynland/sandbox.h>
#include <wynland/vmm.h>
#include <wynland/vma.h>
#include <wynland/types.h>

extern uint64_t timer_get_ms(void);

typedef struct { char *b; uint32_t n, cap; } Out;

static void o_c(Out *o, char c) { if (o->n + 1 < o->cap) o->b[o->n++] = c; }
static void o_s(Out *o, const char *s) { while (*s) o_c(o, *s++); }
static void o_u(Out *o, uint64_t v)
{
    char d[24];
    int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) o_c(o, d[--k]);
}
static void o_i(Out *o, int64_t v) { if (v < 0) { o_c(o, '-'); v = -v; } o_u(o, (uint64_t)v); }
static void o_su(Out *o, uint64_t v) { o_c(o, ' '); o_u(o, v); }
static void o_si(Out *o, int64_t v) { o_c(o, ' '); o_i(o, v); }

static bool same(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* "/proc/PID/..." (or self, thread-self): the process -- one the reader
   can see -- and what follows its directory ("" for the directory) */
Process *procfs_target(const char *path, const char **rest)
{
    const char *pre = "/proc/";
    int i = 0;
    while (pre[i]) { if (path[i] != pre[i]) return NULL; i++; }
    const char *q = path + i;
    Process *me = sched_current() ? sched_current()->proc : NULL;
    Process *p = NULL;
    if (q[0] == 's' && q[1] == 'e' && q[2] == 'l' && q[3] == 'f' && (q[4] == '/' || !q[4])) {
        p = me;
        q += 4;
    } else if (q[0] == 't' && q[1] == 'h' && q[2] == 'r' && q[3] == 'e' && q[4] == 'a' && q[5] == 'd' &&
               q[6] == '-' && q[7] == 's' && q[8] == 'e' && q[9] == 'l' && q[10] == 'f' && (q[11] == '/' || !q[11])) {
        p = me;
        q += 11;
    } else if (*q >= '1' && *q <= '9') {
        uint64_t v = 0;
        while (*q >= '0' && *q <= '9') { v = v * 10 + (uint64_t)(*q - '0'); if (v > 0xFFFFFFFFull) return NULL; q++; }
        if (*q && *q != '/') return NULL;
        uint64_t g = pid_from_ns(v, me);
        p = g ? process_find_by_pid(g) : NULL;
    } else {
        return NULL;
    }
    if (!p || p->reaped) return NULL;
    while (*q == '/') q++;
    if (rest) *rest = q;
    return p;
}

/* the next process the reader sees with a PID (its own numbering) of at
   least `from`: that PID, 0 when there is none -- /proc's listing */
uint64_t procfs_next_pid(uint64_t from)
{
    Process *me = sched_current() ? sched_current()->proc : NULL;
    uint64_t best = 0;
    for (Process *p = process_list_head(); p; p = p->next) {
        if (p->pid == 0 || p->reaped) continue;
        uint64_t l = pid_to_ns(p->pid, me);
        if (!l || l < from) continue;
        if (!best || l < best) best = l;
    }
    return best;
}

/* argv[0]'s last part, at most 15 bytes (Linux's TASK_COMM_LEN - 1) */
static void comm_of(const Process *p, char *c)
{
    const char *b = p->cmdline;
    if (!p->cmdline_len) b = p->exe_path;
    for (const char *q = b; *q; q++) if (*q == '/') b = q + 1;
    int n = 0;
    while (b[n] && n < 15) { c[n] = b[n]; n++; }
    c[n] = 0;
}

static char state_of(const Process *p)
{
    if (p->exited) return 'Z';
    Thread *t = sched_current();
    return (t && t->proc == p) ? 'R' : 'S';
}

static uint64_t vsize_of(const Process *p)
{
    uint64_t v = 0;
    for (const VMA *m = p->vma_list; m; m = m->next) v += m->end - m->start;
    if (p->brk_current > p->brk_start) v += p->brk_current - p->brk_start;
    return v;
}

/* a PTY's device number (major 136: /dev/pts/N), 0 without one */
static uint64_t tty_nr(const Process *p)
{
    if (p->ctty <= 0) return 0;
    uint64_t minor = (uint64_t)(p->ctty - 1);
    return (minor & 0xFF) | (136u << 8) | ((minor & ~0xFFull) << 12);
}

/* /proc/PID/<leaf>'s bytes; false: no such file. cmdline has NULs. */
bool procfs_file(Process *p, const char *leaf, char *buf, uint32_t cap, uint32_t *len)
{
    Process *me = sched_current() ? sched_current()->proc : NULL;
    Out o = { buf, 0, cap };
    char comm[16];
    comm_of(p, comm);
    uint64_t pid = pid_to_ns(p->pid, me);
    uint64_t ppid = pid_to_ns(p->ppid, me), pgrp = pid_to_ns(p->pgid, me), sid = pid_to_ns(p->sid, me);
    uint64_t rss = p->exited ? 0 : vmm_count_user_pages(p->pml4);
    uint64_t vsz = p->exited ? 0 : vsize_of(p);
    int nice = 0;   /* (per thread here; main_thread may be gone already) */

    if (same(leaf, "cmdline")) {
        uint32_t n = p->cmdline_len < cap ? p->cmdline_len : cap;
        for (uint32_t i = 0; i < n; i++) buf[i] = p->cmdline[i];
        *len = n;
        return true;
    }
    if (same(leaf, "comm")) {
        o_s(&o, comm); o_c(&o, '\n');
    } else if (same(leaf, "stat")) {
        /* the 52 fields of proc(5); times in clock ticks (100 a second) */
        o_u(&o, pid); o_s(&o, " ("); o_s(&o, comm); o_s(&o, ") ");
        o_c(&o, state_of(p));
        o_su(&o, ppid); o_su(&o, pgrp); o_su(&o, sid); o_su(&o, tty_nr(p));
        o_si(&o, -1);                                      /* tpgid */
        o_su(&o, 0);                                       /* flags */
        o_su(&o, p->st_faults); o_su(&o, 0); o_su(&o, 0); o_su(&o, 0);   /* minflt cminflt majflt cmajflt */
        o_su(&o, p->st_user_ticks / 10); o_su(&o, p->st_kernel_ticks / 10);
        o_su(&o, 0); o_su(&o, 0);                          /* cutime cstime */
        o_si(&o, 20 + nice); o_si(&o, nice);               /* priority nice */
        o_su(&o, p->thread_count > 0 ? (uint64_t)p->thread_count : 1);
        o_su(&o, 0);                                       /* itrealvalue */
        o_su(&o, p->start_ms / 10);                        /* starttime */
        o_su(&o, vsz); o_su(&o, rss);
        o_su(&o, 18446744073709551615ull);                 /* rsslim */
        for (int k = 26; k <= 37; k++) o_su(&o, 0);        /* startcode .. cnswap */
        o_su(&o, 17);                                      /* exit_signal: SIGCHLD */
        for (int k = 39; k <= 43; k++) o_su(&o, 0);        /* processor .. cguest_time */
        o_su(&o, 0); o_su(&o, 0);                          /* start_data end_data */
        o_su(&o, p->brk_start);                            /* start_brk */
        for (int k = 48; k <= 51; k++) o_su(&o, 0);        /* arg/env start/end */
        o_si(&o, p->exited ? p->wait_status : 0);          /* exit_code */
        o_c(&o, '\n');
    } else if (same(leaf, "statm")) {
        o_u(&o, vsz / 4096); o_su(&o, rss); o_su(&o, 0); o_su(&o, 0); o_su(&o, 0); o_su(&o, 0); o_su(&o, 0);
        o_c(&o, '\n');
    } else if (same(leaf, "status")) {
        static const char *const names[] = { "R (running)", "S (sleeping)", "Z (zombie)" };
        char st = state_of(p);
        o_s(&o, "Name:\t"); o_s(&o, comm);
        o_s(&o, "\nUmask:\t0"); o_u(&o, (p->umask_set ? p->umask : 022) >> 6 & 7);
        o_u(&o, (p->umask_set ? p->umask : 022) >> 3 & 7); o_u(&o, (p->umask_set ? p->umask : 022) & 7);
        o_s(&o, "\nState:\t"); o_s(&o, st == 'R' ? names[0] : st == 'Z' ? names[2] : names[1]);
        o_s(&o, "\nTgid:\t"); o_u(&o, pid);
        o_s(&o, "\nNgid:\t0\nPid:\t"); o_u(&o, pid);
        o_s(&o, "\nPPid:\t"); o_u(&o, ppid);
        o_s(&o, "\nTracerPid:\t0\nUid:\t");
        o_u(&o, p->ruid); o_c(&o, '\t'); o_u(&o, p->uid); o_c(&o, '\t'); o_u(&o, p->suid); o_c(&o, '\t'); o_u(&o, p->uid);
        o_s(&o, "\nGid:\t");
        o_u(&o, p->rgid); o_c(&o, '\t'); o_u(&o, p->gid); o_c(&o, '\t'); o_u(&o, p->sgid); o_c(&o, '\t'); o_u(&o, p->gid);
        o_s(&o, "\nFDSize:\t"); o_u(&o, MAX_OPEN_FILES);
        o_s(&o, "\nGroups:\t");
        for (uint32_t k = 0; k < p->ngroups && k < 32; k++) { o_u(&o, p->groups[k]); o_c(&o, ' '); }
        o_s(&o, "\nVmSize:\t"); o_u(&o, vsz / 1024); o_s(&o, " kB");
        o_s(&o, "\nVmRSS:\t"); o_u(&o, rss * 4); o_s(&o, " kB");
        o_s(&o, "\nThreads:\t"); o_u(&o, p->thread_count > 0 ? (uint64_t)p->thread_count : 1);
        o_s(&o, "\nNoNewPrivs:\t"); o_u(&o, p->no_new_privs ? 1 : 0);
        o_s(&o, "\nSeccomp:\t"); o_u(&o, p->seccomp_mode);
        o_c(&o, '\n');
    } else {
        return false;
    }
    *len = o.n;
    return true;
}
