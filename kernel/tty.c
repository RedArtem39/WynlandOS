/*
 * WynlandOS - pseudo-terminals and their line discipline
 * (include/wynland/tty.h)
 */

#include <wynland/tty.h>
#include <wynland/types.h>
#include <wynland/heap.h>
#include <wynland/sched.h>
#include <wynland/process.h>
#include <wynland/signal.h>
#include <wynland/waitqueue.h>
#include <wynland/usercopy.h>

extern uint64_t timer_get_ms(void);
extern WaitQueue g_poll_any_wq;

#define PTY_BUF   4096
#define PTY_LINE  4096

/* struct termios as the Linux kernel has it (TCGETS/TCSETS): glibc
   converts from its own, larger one */
#define KNCCS 19
typedef struct {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[KNCCS];
} KTermios;
_Static_assert(sizeof(KTermios) == 36, "kernel termios layout");

typedef struct { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; } KWinsize;

/* c_cc indices */
enum { VINTR_ = 0, VQUIT_ = 1, VERASE_ = 2, VKILL_ = 3, VEOF_ = 4, VTIME_ = 5, VMIN_ = 6,
       VSTART_ = 8, VSTOP_ = 9, VSUSP_ = 10, VEOL_ = 11, VREPRINT_ = 12, VDISCARD_ = 13,
       VWERASE_ = 14, VLNEXT_ = 15, VEOL2_ = 16 };
/* flags (octal, as in <asm-generic/termbits.h>) */
#define I_INLCR  0000100
#define I_IGNCR  0000200
#define I_ICRNL  0000400
#define I_IXON   0002000
#define I_IUTF8  0040000
#define O_OPOST  0000001
#define O_ONLCR  0000004
#define C_B38400 0000017
#define C_CS8    0000060
#define C_CREAD  0000200
#define C_HUPCL  0002000
#define L_ISIG   0000001
#define L_ICANON 0000002
#define L_ECHO   0000010
#define L_ECHOE  0000020
#define L_ECHOK  0000040
#define L_ECHONL 0000100
#define L_NOFLSH 0000200
#define L_ECHOCTL 0001000
#define L_ECHOKE 0004000
#define L_IEXTEN 0100000

typedef struct Pty {
    uint8_t  in[PTY_BUF];          /* input the slave reads (cooked) */
    uint32_t in_head, in_count;
    uint8_t  out[PTY_BUF];         /* output the master reads */
    uint32_t out_head, out_count;
    uint8_t  line[PTY_LINE];       /* the canonical line being edited */
    uint32_t line_len;
    uint32_t eofs;                 /* ^D on an empty line: reads that return 0 */
    KTermios term;
    KWinsize ws;
    int      master_refs, slave_refs;
    bool     hung;                 /* the master is gone */
    bool     slave_opened;         /* a slave was ever open: the master may see -EIO */
    uint64_t session;              /* controlling terminal of this session (0: none) */
    uint64_t fg_pgrp;              /* the foreground process group */
    WaitQueue in_wq;               /* slave readers, master writers */
    WaitQueue out_wq;              /* master readers, slave writers */
} Pty;

static Pty *g_pty[MAX_PTYS];

static Pty *get(int idx) { return (idx >= 0 && idx < MAX_PTYS) ? g_pty[idx] : NULL; }
bool pty_exists(int idx) { return get(idx) != NULL; }

static void wake(Pty *p) {
    waitqueue_wake_all(&p->in_wq);
    waitqueue_wake_all(&p->out_wq);
    waitqueue_wake_all(&g_poll_any_wq);
}

/* a deliverable signal is waiting: a blocked read/write ends with -EINTR */
static bool interrupted(void) {
    Thread *t = sched_current();
    return sched_dying() || (t && (t->sig_pending & ~t->sig_mask));
}

int pty_alloc(void) {
    for (int i = 0; i < MAX_PTYS; i++) {
        if (g_pty[i]) continue;
        Pty *p = (Pty *)kmalloc(sizeof(Pty));
        if (!p) return -12;
        memset(p, 0, sizeof(*p));
        p->term.c_iflag = I_ICRNL | I_IXON | I_IUTF8;
        p->term.c_oflag = O_OPOST | O_ONLCR;
        p->term.c_cflag = C_B38400 | C_CS8 | C_CREAD | C_HUPCL;
        p->term.c_lflag = L_ISIG | L_ICANON | L_ECHO | L_ECHOE | L_ECHOK | L_ECHOCTL | L_ECHOKE | L_IEXTEN;
        static const uint8_t cc[KNCCS] = { 3, 0x1c, 0x7f, 0x15, 4, 0, 1, 0, 0x11, 0x13, 0x1a, 0, 0x12, 0x0f, 0x17, 0x16, 0 };
        memcpy(p->term.c_cc, cc, KNCCS);
        p->ws.ws_row = 24;
        p->ws.ws_col = 80;
        g_pty[i] = p;
        return i;
    }
    return -23; /* -ENFILE */
}

void pty_ref(int idx, bool master) {
    Pty *p = get(idx);
    if (!p) return;
    if (master) p->master_refs++; else { p->slave_refs++; p->slave_opened = true; }
}

/* SIGnal every process of a group; a sleeping one is woken when the
   signal will do something */
static void signal_pgrp(uint64_t pgrp, int sig) {
    if (!pgrp) return;
    for (Process *q = process_list_head(); q; q = q->next) {
        if (q->pgid != pgrp || q->exited || q->pid == 0) continue;
        Thread *t = q->main_thread;
        if (!t || t->state == THREAD_STATE_TERMINATED) continue;
        signal_raise_thread(t->id, q->pid, sig);
        if (t->state == THREAD_STATE_BLOCKED && signal_wants_wake(t, sig)) sched_unblock(t, -4);
    }
}

void pty_unref(int idx, bool master) {
    Pty *p = get(idx);
    if (!p) return;
    if (master) {
        if (p->master_refs > 0 && --p->master_refs == 0) {
            /* hangup: the session's foreground hears of it */
            p->hung = true;
            signal_pgrp(p->fg_pgrp, 1 /* SIGHUP */);
            signal_pgrp(p->fg_pgrp, 18 /* SIGCONT */);
        }
    } else if (p->slave_refs > 0) {
        p->slave_refs--;
    }
    wake(p);
    if (p->master_refs == 0 && p->slave_refs == 0) {
        /* nobody can name it any more: no process keeps it as its terminal */
        for (Process *q = process_list_head(); q; q = q->next)
            if (q->ctty == idx + 1) q->ctty = 0;
        g_pty[idx] = NULL;
        kfree(p);
    }
}

/* ---------------------------------------------------------------- rings */

static bool ring_put(uint8_t *buf, uint32_t head, uint32_t *count, uint8_t c) {
    if (*count >= PTY_BUF) return false;
    buf[(head + *count) % PTY_BUF] = c;
    (*count)++;
    return true;
}

static uint8_t ring_get(uint8_t *buf, uint32_t *head, uint32_t *count) {
    uint8_t c = buf[*head];
    *head = (*head + 1) % PTY_BUF;
    (*count)--;
    return c;
}

/* output towards the master, with OPOST/ONLCR; false when it is full */
static bool out_char(Pty *p, uint8_t c) {
    if ((p->term.c_oflag & O_OPOST) && (p->term.c_oflag & O_ONLCR) && c == '\n') {
        if (p->out_count + 2 > PTY_BUF) return false;
        ring_put(p->out, p->out_head, &p->out_count, '\r');
    }
    return ring_put(p->out, p->out_head, &p->out_count, c);
}

/* echo of one input character: control characters as ^X with ECHOCTL */
static void echo_char(Pty *p, uint8_t c) {
    if (!(p->term.c_lflag & L_ECHO)) {
        if (c == '\n' && (p->term.c_lflag & L_ECHONL) && (p->term.c_lflag & L_ICANON)) out_char(p, c);
        return;
    }
    if ((p->term.c_lflag & L_ECHOCTL) && c < 32 && c != '\n' && c != '\t') {
        out_char(p, '^');
        out_char(p, (uint8_t)(c + '@'));
    } else if ((p->term.c_lflag & L_ECHOCTL) && c == 0x7f) {
        out_char(p, '^');
        out_char(p, '?');
    } else {
        out_char(p, c);
    }
}

/* erase the last character of the line (UTF-8 aware: a whole code point) */
static void erase_one(Pty *p) {
    if (!p->line_len) return;
    uint32_t n = 1;
    if (p->term.c_iflag & I_IUTF8)
        while (n < p->line_len && (p->line[p->line_len - n] & 0xC0) == 0x80) n++;
    uint8_t first = p->line[p->line_len - n];
    p->line_len -= n;
    if ((p->term.c_lflag & L_ECHO) && (p->term.c_lflag & L_ECHOE)) {
        int cols = ((p->term.c_lflag & L_ECHOCTL) && (first < 32 || first == 0x7f)) ? 2 : 1;
        for (int k = 0; k < cols; k++) { out_char(p, '\b'); out_char(p, ' '); out_char(p, '\b'); }
    }
}

/* the edited line goes where the slave reads it */
static void commit_line(Pty *p) {
    for (uint32_t i = 0; i < p->line_len; i++) ring_put(p->in, p->in_head, &p->in_count, p->line[i]);
    p->line_len = 0;
}

/* one character typed (written to the master): false when there is no
   room for it now (raw mode, input full) */
static bool input_char(Pty *p, uint8_t c) {
    KTermios *t = &p->term;
    if (c == '\r') {
        if (t->c_iflag & I_IGNCR) return true;
        if (t->c_iflag & I_ICRNL) c = '\n';
    } else if (c == '\n' && (t->c_iflag & I_INLCR)) {
        c = '\r';
    }
    if (t->c_lflag & L_ISIG) {
        int sig = 0;
        if (c == t->c_cc[VINTR_] && c) sig = 2;
        else if (c == t->c_cc[VQUIT_] && c) sig = 3;
        else if (c == t->c_cc[VSUSP_] && c) sig = 20;
        if (sig) {
            if (!(t->c_lflag & L_NOFLSH)) { p->line_len = 0; p->in_count = 0; p->eofs = 0; }
            echo_char(p, c);
            signal_pgrp(p->fg_pgrp, sig);
            return true;
        }
    }
    if (t->c_lflag & L_ICANON) {
        if (c == t->c_cc[VERASE_] || c == 0x7f || c == '\b') { erase_one(p); return true; }
        if (c == t->c_cc[VKILL_] && c) {
            while (p->line_len) erase_one(p);
            return true;
        }
        if (c == t->c_cc[VWERASE_] && c && (t->c_lflag & L_IEXTEN)) {
            while (p->line_len && p->line[p->line_len - 1] == ' ') erase_one(p);
            while (p->line_len && p->line[p->line_len - 1] != ' ') erase_one(p);
            return true;
        }
        if (c == t->c_cc[VEOF_] && c) {
            if (p->line_len) commit_line(p); else p->eofs++;
            return true;
        }
        bool eol = c == '\n' || (c && (c == t->c_cc[VEOL_] || c == t->c_cc[VEOL2_]));
        /* full: anything but the newline that ends the line is lost (the
           last slot of the line is kept for it) */
        if (p->line_len >= PTY_LINE - 1 && !eol) return true;
        if (p->in_count + p->line_len + 1 > PTY_BUF) return eol ? false : true;
        p->line[p->line_len++] = c;
        echo_char(p, c);
        if (eol) commit_line(p);
        return true;
    }
    if (p->in_count >= PTY_BUF) return false;
    ring_put(p->in, p->in_head, &p->in_count, c);
    echo_char(p, c);
    return true;
}

/* ---------------------------------------------------------------- I/O */

int64_t pty_read(int idx, bool master, void *ubuf, uint32_t n, bool nonblock) {
    Pty *p = get(idx);
    if (!p) return -9;
    if (n == 0) return 0;
    uint8_t kb[256];
    if (master) {
        while (p->out_count == 0) {
            if (p->slave_refs == 0 && p->slave_opened) return -5;   /* -EIO: the other side is gone */
            if (nonblock) return -11;
            if (interrupted()) return -4;
            waitqueue_wait_ms(&p->out_wq, timer_get_ms() + 1000);
            if (!get(idx)) return -9;
        }
        uint32_t done = 0;
        while (done < n && p->out_count) {
            uint32_t k = 0;
            while (k < sizeof(kb) && done + k < n && p->out_count) kb[k++] = ring_get(p->out, &p->out_head, &p->out_count);
            if (copy_to_user((uint8_t *)ubuf + done, kb, k) != 0) return done ? (int64_t)done : -14;
            done += k;
        }
        wake(p);
        return (int64_t)done;
    }

    /* slave */
    bool canon = (p->term.c_lflag & L_ICANON) != 0;
    uint8_t vmin = p->term.c_cc[VMIN_], vtime = p->term.c_cc[VTIME_];
    uint64_t deadline = (!canon && vmin == 0 && vtime) ? timer_get_ms() + (uint64_t)vtime * 100 : 0;
    while (p->in_count == 0) {
        if (p->hung) return 0;
        if (canon && p->eofs) { p->eofs--; return 0; }
        if (!canon && vmin == 0 && (!vtime || timer_get_ms() >= deadline)) return 0;
        if (nonblock) return -11;
        if (interrupted()) return -4;
        waitqueue_wait_ms(&p->in_wq, deadline ? deadline : timer_get_ms() + 1000);
        if (!get(idx)) return -5;
    }
    uint32_t done = 0;
    bool line_end = false;
    while (done < n && p->in_count && !line_end) {
        uint32_t k = 0;
        while (k < sizeof(kb) && done + k < n && p->in_count) {
            uint8_t c = ring_get(p->in, &p->in_head, &p->in_count);
            kb[k++] = c;
            if (canon && (c == '\n' || (c && (c == p->term.c_cc[VEOL_] || c == p->term.c_cc[VEOL2_])))) {
                line_end = true;   /* a canonical read ends with its line */
                break;
            }
        }
        if (copy_to_user((uint8_t *)ubuf + done, kb, k) != 0) return done ? (int64_t)done : -14;
        done += k;
    }
    wake(p);
    return (int64_t)done;
}

int64_t pty_write(int idx, bool master, const void *ubuf, uint32_t n, bool nonblock) {
    Pty *p = get(idx);
    if (!p) return -9;
    uint8_t kb[256];
    uint32_t done = 0;
    while (done < n) {
        uint32_t k = n - done < sizeof(kb) ? n - done : (uint32_t)sizeof(kb);
        if (copy_from_user(kb, (const uint8_t *)ubuf + done, k) != 0) return done ? (int64_t)done : -14;
        uint32_t i = 0;
        for (; i < k; i++) {
            if (master ? !input_char(p, kb[i]) : !out_char(p, kb[i])) break;
        }
        done += i;
        if (i) wake(p);
        if (i < k) {
            /* full: what went in is reported; with nothing at all, wait */
            if (done) return (int64_t)done;
            if (!master && p->hung) return -5;
            if (nonblock) return -11;
            if (interrupted()) return -4;
            waitqueue_wait_ms(master ? &p->in_wq : &p->out_wq, timer_get_ms() + 1000);
            if (!get(idx)) return -5;
        }
        if (!master && p->hung) return done ? (int64_t)done : -5;
    }
    return (int64_t)done;
}

uint32_t pty_poll(int idx, bool master, uint32_t events, WaitQueue **wq) {
    Pty *p = get(idx);
    if (!p) return 0x0008 /* POLLERR */;
    uint32_t r = 0;
    if (master) {
        if (p->out_count) r |= 0x0001;                   /* POLLIN */
        if (p->slave_refs == 0 && p->slave_opened) r |= 0x0010;   /* POLLHUP */
        if (p->in_count + p->line_len < PTY_BUF) r |= 0x0004;  /* POLLOUT */
        if (wq) *wq = &p->out_wq;
    } else {
        bool canon = (p->term.c_lflag & L_ICANON) != 0;
        if (p->in_count || (canon && p->eofs) || p->hung) r |= 0x0001;
        if (p->hung) r |= 0x0010;
        if (p->out_count + 2 <= PTY_BUF) r |= 0x0004;
        if (wq) *wq = &p->in_wq;
    }
    return r & (events | 0x0018);
}

/* ---------------------------------------------------------------- control */

int pty_ctty(Process *p) {
    if (!p || !p->ctty) return -1;
    return get(p->ctty - 1) ? p->ctty - 1 : -1;
}

static void make_ctty(Pty *pt, int idx, Process *p) {
    pt->session = p->sid;
    pt->fg_pgrp = p->pgid;
    p->ctty = idx + 1;
}

void pty_open_slave(int idx, Process *p, bool noctty) {
    Pty *pt = get(idx);
    if (!pt || !p || noctty) return;
    if (p->pid == p->sid && !p->ctty && !pt->session) make_ctty(pt, idx, p);
}

void pty_detach(Process *p) {
    if (p) p->ctty = 0;
}

int64_t pty_ioctl(int idx, bool master, uint64_t req, uint64_t argp) {
    Pty *p = get(idx);
    if (!p) return -9;
    Process *me = sched_current()->proc;
    switch (req) {
    case 0x5401: /* TCGETS */
        if (copy_to_user((void *)argp, &p->term, sizeof(p->term)) != 0) return -14;
        return 0;
    case 0x5402: case 0x5403: case 0x5404: { /* TCSETS, TCSETSW, TCSETSF */
        KTermios nt;
        if (copy_from_user(&nt, (const void *)argp, sizeof(nt)) != 0) return -14;
        if (req == 0x5404) { p->in_count = 0; p->line_len = 0; p->eofs = 0; }
        /* leaving canonical mode: a half-typed line becomes readable */
        if ((p->term.c_lflag & L_ICANON) && !(nt.c_lflag & L_ICANON)) commit_line(p);
        p->term = nt;
        wake(p);
        return 0;
    }
    case 0x802C542A: { /* TCGETS2: termios + input/output speeds (glibc 2.42's tcgetattr) */
        struct { KTermios t; uint32_t ispeed, ospeed; } __attribute__((packed)) t2 = { p->term, 38400, 38400 };
        _Static_assert(sizeof(t2) == 44, "termios2 layout");
        if (copy_to_user((void *)argp, &t2, sizeof(t2)) != 0) return -14;
        return 0;
    }
    case 0x402C542B: case 0x402C542C: case 0x402C542D: { /* TCSETS2, TCSETSW2, TCSETSF2 */
        KTermios nt;
        if (copy_from_user(&nt, (const void *)argp, sizeof(nt)) != 0) return -14;
        if (req == 0x402C542D) { p->in_count = 0; p->line_len = 0; p->eofs = 0; }
        if ((p->term.c_lflag & L_ICANON) && !(nt.c_lflag & L_ICANON)) commit_line(p);
        p->term = nt;
        wake(p);
        return 0;
    }
    case 0x5413: /* TIOCGWINSZ */
        if (copy_to_user((void *)argp, &p->ws, sizeof(p->ws)) != 0) return -14;
        return 0;
    case 0x5414: { /* TIOCSWINSZ */
        KWinsize w;
        if (copy_from_user(&w, (const void *)argp, sizeof(w)) != 0) return -14;
        bool changed = w.ws_row != p->ws.ws_row || w.ws_col != p->ws.ws_col;
        p->ws = w;
        if (changed) signal_pgrp(p->fg_pgrp, 28 /* SIGWINCH */);
        return 0;
    }
    case 0x540F: { /* TIOCGPGRP */
        int32_t g = (int32_t)p->fg_pgrp;
        if (!master && pty_ctty(me) != idx) return -25;
        if (copy_to_user((void *)argp, &g, 4) != 0) return -14;
        return 0;
    }
    case 0x5410: { /* TIOCSPGRP */
        int32_t g;
        if (copy_from_user(&g, (const void *)argp, 4) != 0) return -14;
        if (g <= 0) return -22;
        if (!master && pty_ctty(me) != idx) return -25;
        p->fg_pgrp = (uint64_t)g;
        return 0;
    }
    case 0x5429: { /* TIOCGSID */
        int32_t s = (int32_t)p->session;
        if (!s) return -25;
        if (copy_to_user((void *)argp, &s, 4) != 0) return -14;
        return 0;
    }
    case 0x540E: /* TIOCSCTTY */
        if (master) return -25;
        if (me->pid != me->sid) return -1;               /* -EPERM: not a session leader */
        if (pty_ctty(me) == idx) return 0;
        if (p->session && p->session != me->sid && !(argp == 1 && me->uid == 0)) return -1;
        make_ctty(p, idx, me);
        return 0;
    case 0x5422: /* TIOCNOTTY */
        if (pty_ctty(me) != idx) return -25;
        if (me->pid == me->sid) { p->session = 0; p->fg_pgrp = 0; }
        me->ctty = 0;
        return 0;
    case 0x541B: { /* FIONREAD */
        int32_t v = (int32_t)(master ? p->out_count : p->in_count);
        if (copy_to_user((void *)argp, &v, 4) != 0) return -14;
        return 0;
    }
    case 0x5411: { /* TIOCOUTQ */
        int32_t v = (int32_t)(master ? p->in_count : p->out_count);
        if (copy_to_user((void *)argp, &v, 4) != 0) return -14;
        return 0;
    }
    case 0x540B: /* TCFLSH: 0 input, 1 output, 2 both */
        if (argp == 0 || argp == 2) { p->in_count = 0; p->line_len = 0; p->eofs = 0; }
        if (argp == 1 || argp == 2) p->out_count = 0;
        wake(p);
        return 0;
    case 0x80045430: { /* TIOCGPTN */
        if (!master) return -25;
        uint32_t v = (uint32_t)idx;
        if (copy_to_user((void *)argp, &v, 4) != 0) return -14;
        return 0;
    }
    case 0x80045439: { /* TIOCGPTLCK */
        uint32_t v = 0;
        if (copy_to_user((void *)argp, &v, 4) != 0) return -14;
        return 0;
    }
    case 0x5424: { /* TIOCGETD: N_TTY */
        uint32_t v = 0;
        if (copy_to_user((void *)argp, &v, 4) != 0) return -14;
        return 0;
    }
    case 0x40045431: /* TIOCSPTLCK */
    case 0x5409: case 0x5425: case 0x5427: case 0x5428:  /* TCSBRK, TCSBRKP, TIOCSBRK, TIOCCBRK */
    case 0x540A: /* TCXONC */
    case 0x540C: case 0x540D: /* TIOCEXCL, TIOCNXCL */
    case 0x5423: /* TIOCSETD */
        return 0;
    }
    return -25; /* -ENOTTY */
}
