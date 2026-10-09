/*
 * WynlandOS - AF_UNIX sockets (stream, seqpacket, datagram) with
 * SCM_RIGHTS fd passing.
 *
 * Each socket is a slot in g_usock[]; fds reference it through a VfsFile
 * wrapper (first_cluster = USOCK_FD, current_cluster = slot) and the slot
 * counts those wrappers (see include/wynland/kfile.h). The last close
 * tears the socket down: the peer is detached (its reads see EOF, its
 * writes EPIPE), queued messages are dropped -- closing any fds still in
 * flight inside them -- and a bound name is released.
 *
 * Data lives in a per-socket receive queue of messages. A sender appends
 * to its PEER's queue; one message carries the bytes of one send (stream
 * sends may be merged into the tail message when neither carries fds) and
 * the fds passed with it. Stream reads cross message boundaries, except
 * that a message carrying fds is never merged into bytes already read
 * (Linux does the same, so each fd batch arrives with the byte it was
 * sent with). Seqpacket/datagram reads return exactly one message.
 *
 * Everything runs with interrupts off (syscall context, SFMASK), so the
 * only points where another thread can run are the waitqueue_wait_ms()
 * calls. Every blocking entry point holds its own temporary reference so
 * a concurrent close() of the fd from another thread can't free the slot
 * out from under the sleeper.
 *
 * Wake-ups: each socket has ONE wait queue, woken for anything that can
 * change its poll state -- data arriving in its queue, its peer consuming
 * data (room to write again), a peer disappearing, a pending connection
 * for a listener.
 */
#include <wynland/types.h>
#include <wynland/heap.h>
#include <wynland/sched.h>
#include <wynland/process.h>
#include <wynland/signal.h>
#include <wynland/kfile.h>
#include <wynland/unix_socket.h>
#include <wynland/sandbox.h>

extern uint64_t timer_get_ms(void);

#define USOCK_SLOTS     256
#define USOCK_RCVBUF    (208 * 1024)   /* Linux's default net.core.rmem_default */
#define USOCK_BACKLOG   64
#define USOCK_NAME_MAX  108            /* sizeof(sun_path) */
#define UMSG_MIN_CAP    512
/* Queued messages per socket, whatever their size: rx_bytes counts only
   payload, so a flood of empty or 1-byte messages (each a UMsg plus
   UMSG_MIN_CAP of kernel heap) used to be unbounded and could exhaust the
   kernel heap for the whole system. */
#define USOCK_MAX_MSGS  1024

#define EAGAIN      11
#define ENOMEM      12
#define EFAULT      14
#define EINVAL      22
#define ENFILE      23
#define EPIPE       32
#define ENOENT       2
#define ENOTSOCK    88
#define EDESTADDRREQ 89
#define EMSGSIZE    90
#define EOPNOTSUPP  95
#define EADDRINUSE  98
#define ECONNRESET 104
#define EISCONN    106
#define ENOTCONN   107
#define ECONNREFUSED 111

enum { US_UNCONNECTED, US_LISTENING, US_CONNECTED };

typedef struct UMsg {
    struct UMsg *next;
    uint32_t len;          /* bytes of data held */
    uint32_t off;          /* bytes already consumed (stream) */
    uint32_t cap;          /* allocated data capacity */
    uint32_t nfds;
    VfsFile *fds[USOCK_MAX_MSG_FDS];
    uint16_t src_len;      /* datagram sender's bound name, for recvfrom */
    char     src[USOCK_NAME_MAX];
    uint8_t  data[];
} UMsg;

typedef struct USock {
    int      idx;
    int      type;
    int      state;
    int      refs;          /* VfsFile wrappers + temporary op references */
    int      backlog;
    struct USock *peer;
    bool     was_connected; /* peer went away (EOF/EPIPE rather than ENOTCONN) */
    bool     rd_shut;       /* shutdown(SHUT_RD) by us, or peer shut writing */
    bool     wr_shut;       /* shutdown(SHUT_WR) by us */
    bool     peer_wr_shut;  /* peer did shutdown(SHUT_WR): EOF after the queue */
    UMsg    *rx_head, *rx_tail;
    uint32_t rx_bytes;
    uint32_t rx_msgs;       /* messages queued (bounded by USOCK_MAX_MSGS) */
    struct USock *pend[USOCK_BACKLOG]; /* server ends waiting in accept() */
    int      npend;
    bool     bound;
    uint16_t name_len;
    uint32_t scope;                /* whose name it is: name_scope() */
    char     name[USOCK_NAME_MAX];
    struct UCred cred;       /* creator's */
    struct UCred peer_cred;  /* peer's, captured at connect/socketpair time */
    WaitQueue wq;
} USock;

static USock *g_usock[USOCK_SLOTS];

/* ---------------------------------------------------------------- */

static void kmemcpy(void *d, const void *s, uint64_t n) { memcpy(d, s, (size_t)n); }

static struct UCred current_cred(void) {
    Process *p = sched_current()->proc;
    struct UCred c;
    c.pid = (int32_t)p->pid;
    c.uid = p->uid;
    c.gid = p->gid;
    return c;
}

static USock *slot(int idx) {
    if (idx < 0 || idx >= USOCK_SLOTS) return NULL;
    return g_usock[idx];
}

static USock *usock_alloc(int type) {
    for (int i = 0; i < USOCK_SLOTS; i++) {
        if (g_usock[i]) continue;
        USock *s = (USock *)kmalloc(sizeof(USock));
        if (!s) return NULL;
        memset(s, 0, sizeof(USock));
        s->idx = i;
        s->type = type;
        s->state = US_UNCONNECTED;
        s->cred = current_cred();
        g_usock[i] = s;
        return s;
    }
    return NULL;
}

static void msg_free(UMsg *m) {
    for (uint32_t i = 0; i < m->nfds; i++) {
        if (m->fds[i]) kfile_close(m->fds[i]);
    }
    kfree(m);
}

WaitQueue g_poll_any_wq;

static void wake(USock *s) {
    if (s) waitqueue_wake_all(&s->wq);
    waitqueue_wake_all(&g_poll_any_wq);
}

static void usock_destroy(USock *s);

/* Detach s from its peer: the peer sees EOF / EPIPE from now on. */
static void disconnect(USock *s) {
    USock *p = s->peer;
    if (!p) return;
    s->peer = NULL;
    if (p->peer == s) {
        p->peer = NULL;
        p->was_connected = true;
        wake(p);
    }
}

static void usock_destroy(USock *s) {
    g_usock[s->idx] = NULL;
    disconnect(s);
    /* A datagram socket connect()ed to s points at it one-directionally
       (s->peer doesn't point back): clear those too. */
    for (int i = 0; i < USOCK_SLOTS; i++) {
        USock *o = g_usock[i];
        if (o && o->peer == s) {
            o->peer = NULL;
            o->was_connected = true;
            wake(o);
        }
    }
    /* Connections nobody accepted yet: their clients see EOF/ECONNRESET. */
    for (int i = 0; i < s->npend; i++) {
        USock *c = s->pend[i];
        disconnect(c);
        UMsg *m = c->rx_head;
        while (m) { UMsg *n = m->next; msg_free(m); m = n; }
        g_usock[c->idx] = NULL;
        kfree(c);
    }
    s->npend = 0;
    UMsg *m = s->rx_head;
    s->rx_head = s->rx_tail = NULL;
    s->rx_msgs = 0;
    while (m) { UMsg *n = m->next; msg_free(m); m = n; }
    wake(s); /* anyone still parked here holds a ref -- can't happen, but harmless */
    kfree(s);
}

void usock_ref(int idx) {
    USock *s = slot(idx);
    if (s) s->refs++;
}

void usock_unref(int idx) {
    USock *s = slot(idx);
    if (!s) return;
    if (--s->refs <= 0) usock_destroy(s);
}

int usock_type(int idx) {
    USock *s = slot(idx);
    return s ? s->type : -1;
}

int usock_create(int type) {
    if (type != USOCK_STREAM && type != USOCK_SEQPACKET && type != USOCK_DGRAM) return -EINVAL;
    USock *s = usock_alloc(type);
    if (!s) return -ENFILE;
    s->refs = 1;
    return s->idx;
}

static void link_pair(USock *a, USock *b) {
    a->peer = b; b->peer = a;
    a->state = b->state = US_CONNECTED;
    a->was_connected = b->was_connected = true;
    a->peer_cred = b->cred;
    b->peer_cred = a->cred;
}

int usock_pair(int type, int out[2]) {
    int a = usock_create(type);
    if (a < 0) return a;
    int b = usock_create(type);
    if (b < 0) { usock_unref(a); return b; }
    link_pair(g_usock[a], g_usock[b]);
    out[0] = a; out[1] = b;
    return 0;
}

/* ---- names ---- */

static bool name_eq(const USock *s, const char *name, uint32_t len) {
    if (!s->bound || s->name_len != len) return false;
    for (uint32_t i = 0; i < len; i++) if (s->name[i] != name[i]) return false;
    return true;
}

/* Whose names: a filesystem name belongs to the root it was bound under
   (a chroot's "/tmp/x" is not the host's), an abstract one to the network
   namespace (as on Linux). */
static uint32_t name_scope(const char *name, uint32_t len) {
    if (len && name[0] == '\0') return 0x80000000u | sandbox_netns(sched_current()->proc);
    return sandbox_lookup_root();
}

static USock *find_bound(const char *name, uint32_t len) {
    uint32_t scope = name_scope(name, len);
    for (int i = 0; i < USOCK_SLOTS; i++) {
        if (g_usock[i] && g_usock[i]->scope == scope && name_eq(g_usock[i], name, len)) return g_usock[i];
    }
    return NULL;
}

/* Filesystem names compare up to their NUL (sun_path may carry padding);
   abstract names (leading NUL) are exactly `len` bytes. */
static uint32_t norm_name_len(const char *name, uint32_t len) {
    if (len > USOCK_NAME_MAX) len = USOCK_NAME_MAX;
    if (len == 0 || name[0] == '\0') return len;
    for (uint32_t i = 0; i < len; i++) if (name[i] == '\0') return i;
    return len;
}

int64_t usock_bind(int idx, const char *name, uint32_t len) {
    USock *s = slot(idx);
    if (!s) return -ENOTSOCK;
    len = norm_name_len(name, len);
    if (len == 0) return -EINVAL; /* autobind isn't implemented */
    if (s->bound) return -EINVAL;
    if (find_bound(name, len)) return -EADDRINUSE;
    kmemcpy(s->name, name, len);
    s->name_len = (uint16_t)len;
    s->scope = name_scope(name, len);
    s->bound = true;
    return 0;
}

int64_t usock_listen(int idx, int backlog) {
    USock *s = slot(idx);
    if (!s) return -ENOTSOCK;
    if (s->type == USOCK_DGRAM) return -EOPNOTSUPP;
    if (s->state == US_CONNECTED) return -EINVAL;
    if (!s->bound) return -EINVAL; /* no autobind */
    if (backlog <= 0 || backlog > USOCK_BACKLOG) backlog = USOCK_BACKLOG;
    s->backlog = backlog;
    s->state = US_LISTENING;
    return 0;
}

int64_t usock_connect(int idx, const char *name, uint32_t len, bool nonblock) {
    USock *s = slot(idx);
    if (!s) return -ENOTSOCK;
    len = norm_name_len(name, len);
    if (len == 0) return -EINVAL;

    if (s->type == USOCK_DGRAM) {
        /* Datagram "connect" just fixes the default destination. */
        USock *t = find_bound(name, len);
        if (!t || t->type != USOCK_DGRAM) return t ? -ECONNREFUSED : -ENOENT;
        s->peer = t;          /* one-directional: t->peer is untouched */
        s->state = US_CONNECTED;
        s->peer_cred = t->cred;
        return 0;
    }

    if (s->state == US_CONNECTED) return -EISCONN;
    if (s->state == US_LISTENING) return -EINVAL;

    s->refs++; /* held across the wait below */
    int64_t ret;
    for (;;) {
        USock *l = find_bound(name, len);
        if (!l) { ret = name[0] ? -ENOENT : -ECONNREFUSED; break; }
        if (l->state != US_LISTENING || l->type != s->type) { ret = -ECONNREFUSED; break; }
        if (l->npend < l->backlog) {
            USock *srv = usock_alloc(s->type);
            if (!srv) { ret = -ENFILE; break; }
            srv->cred = l->cred;           /* server end belongs to the listener's owner */
            kmemcpy(srv->name, l->name, l->name_len);
            srv->name_len = l->name_len;   /* getsockname() on the accepted fd */
            srv->scope = l->scope;
            link_pair(s, srv);
            srv->refs = 0;                 /* owned by the listener until accept() */
            l->pend[l->npend++] = srv;
            wake(l);
            ret = 0;
            break;
        }
        if (nonblock) { ret = -EAGAIN; break; }
        if (sched_dying()) { ret = -4; break; }   /* -EINTR */
        l->refs++;
        waitqueue_wait_ms(&l->wq, SCHED_NO_DEADLINE);
        usock_unref(l->idx);
    }
    usock_unref(idx);
    return ret;
}

int64_t usock_accept(int idx, bool nonblock) {
    USock *s = slot(idx);
    if (!s) return -ENOTSOCK;
    if (s->state != US_LISTENING) return -EINVAL;
    s->refs++;
    int64_t ret;
    for (;;) {
        if (s->npend > 0) {
            USock *c = s->pend[0];
            for (int i = 1; i < s->npend; i++) s->pend[i - 1] = s->pend[i];
            s->npend--;
            c->refs = 1;
            wake(s); /* a connect() blocked on a full backlog can proceed */
            ret = c->idx;
            break;
        }
        if (nonblock) { ret = -EAGAIN; break; }
        if (sched_dying()) { ret = -4; break; }   /* -EINTR */
        waitqueue_wait_ms(&s->wq, SCHED_NO_DEADLINE);
        if (!g_usock[idx]) { ret = -EINVAL; break; } /* can't happen: we hold a ref */
    }
    usock_unref(idx);
    return ret;
}

/* ---- data ---- */

static uint64_t iov_total_r(const UIoVecR *iov, int n) {
    uint64_t t = 0;
    for (int i = 0; i < n; i++) t += iov[i].len;
    return t;
}

/* Copy `len` bytes starting at logical offset `skip` of the iovec list. */
static void iov_gather(uint8_t *dst, const UIoVecR *iov, int n, uint64_t skip, uint64_t len) {
    for (int i = 0; i < n && len > 0; i++) {
        if (skip >= iov[i].len) { skip -= iov[i].len; continue; }
        uint64_t take = iov[i].len - skip;
        if (take > len) take = len;
        kmemcpy(dst, iov[i].base + skip, take);
        dst += take; len -= take; skip = 0;
    }
}

static void enqueue(USock *r, UMsg *m) {
    m->next = NULL;
    if (r->rx_tail) r->rx_tail->next = m; else r->rx_head = m;
    r->rx_tail = m;
    r->rx_bytes += m->len;
    r->rx_msgs++;
}

/* one message left the queue: room for a writer again */
static void dequeued(USock *s) {
    if (s->rx_msgs) s->rx_msgs--;
    if (s->peer) wake(s->peer);
}

static UMsg *msg_new(uint32_t cap) {
    UMsg *m = (UMsg *)kmalloc(sizeof(UMsg) + cap);
    if (!m) return NULL;
    memset(m, 0, sizeof(UMsg));
    m->cap = cap;
    return m;
}

static int64_t epipe(int flags) {
    if (!(flags & UMSG_NOSIGNAL)) signal_raise_current(13 /* SIGPIPE */);
    return -EPIPE;
}

int64_t usock_send(int idx, const UIoVecR *iov, int iovcnt, VfsFile **fds, uint32_t nfds,
                   int flags, bool nonblock, const char *dest, uint32_t dest_len) {
    USock *s = slot(idx);
    if (!s) return -ENOTSOCK;
    if (flags & UMSG_DONTWAIT) nonblock = true;
    uint64_t total = iov_total_r(iov, iovcnt);
    if (nfds > USOCK_MAX_MSG_FDS) return -EINVAL;

    s->refs++;
    int64_t ret = 0;
    uint64_t sent = 0;
    bool fds_sent = false;

    for (;;) {
        if (s->wr_shut) { ret = epipe(flags); break; }

        USock *r;
        if (s->type == USOCK_DGRAM && dest) {
            uint32_t dl = norm_name_len(dest, dest_len);
            r = find_bound(dest, dl);
            if (!r || r->type != USOCK_DGRAM) { ret = r ? -ECONNREFUSED : -ENOENT; break; }
        } else {
            r = s->peer;
            if (!r) {
                if (s->type == USOCK_DGRAM) ret = s->state == US_CONNECTED ? -ECONNREFUSED : -EDESTADDRREQ;
                else ret = s->was_connected ? epipe(flags) : -ENOTCONN;
                break;
            }
            if (r->rd_shut && s->type != USOCK_DGRAM) { ret = epipe(flags); break; }
        }

        uint32_t room = r->rx_bytes >= USOCK_RCVBUF ? 0 : USOCK_RCVBUF - r->rx_bytes;
        const bool msg_room = r->rx_msgs < USOCK_MAX_MSGS;

        if (s->type == USOCK_STREAM) {
            uint64_t left = total - sent;
            if (left == 0 && (fds_sent || nfds == 0)) { ret = (int64_t)sent; break; }
            uint32_t chunk = (uint32_t)(left < room ? left : room);
            bool with_fds = !fds_sent && nfds > 0;
            UMsg *tail = r->rx_tail;
            bool append = !with_fds && tail && tail->nfds == 0 && tail->cap - tail->len >= chunk && chunk > 0;
            /* a new message needs a free queue slot; without one, wait */
            if ((room > 0 || (left == 0 && !fds_sent)) && (append || msg_room)) {
                if (append) {
                    iov_gather(tail->data + tail->len, iov, iovcnt, sent, chunk);
                    tail->len += chunk;
                    r->rx_bytes += chunk;
                } else {
                    uint32_t cap = chunk < UMSG_MIN_CAP ? UMSG_MIN_CAP : chunk;
                    UMsg *m = msg_new(cap);
                    if (!m) { ret = sent ? (int64_t)sent : -ENOMEM; break; }
                    iov_gather(m->data, iov, iovcnt, sent, chunk);
                    m->len = chunk;
                    if (with_fds) {
                        for (uint32_t i = 0; i < nfds; i++) m->fds[i] = fds[i];
                        m->nfds = nfds;
                        fds_sent = true;
                    }
                    enqueue(r, m);
                }
                sent += chunk;
                wake(r);
                continue;
            }
            if (nonblock) { ret = sent ? (int64_t)sent : -EAGAIN; break; }
        } else {
            /* seqpacket / datagram: one atomic message */
            if (total > USOCK_RCVBUF) { ret = -EMSGSIZE; break; }
            if (msg_room && (room >= total || r->rx_head == NULL)) {
                UMsg *m = msg_new((uint32_t)total);
                if (!m) { ret = -ENOMEM; break; }
                iov_gather(m->data, iov, iovcnt, 0, total);
                m->len = (uint32_t)total;
                for (uint32_t i = 0; i < nfds; i++) m->fds[i] = fds[i];
                m->nfds = nfds;
                fds_sent = true;
                if (s->bound) { kmemcpy(m->src, s->name, s->name_len); m->src_len = s->name_len; }
                enqueue(r, m);
                wake(r);
                ret = (int64_t)total;
                break;
            }
            if (nonblock) { ret = -EAGAIN; break; }
        }

        /* Wait for the receiver to drain: it wakes its peer (us) on every
           consumption. A datagram target that isn't our peer wakes its own
           queue; wait there instead. */
        if (sched_dying()) { ret = -4; break; }   /* -EINTR */
        USock *wq_owner = (r->peer == s) ? s : r;
        wq_owner->refs++;
        r->refs++;
        waitqueue_wait_ms(&wq_owner->wq, SCHED_NO_DEADLINE);
        usock_unref(r->idx);
        usock_unref(wq_owner->idx);
    }

    /* fds the caller handed us are ours now: close any we didn't queue */
    if (!fds_sent) {
        for (uint32_t i = 0; i < nfds; i++) if (fds[i]) kfile_close(fds[i]);
    }
    usock_unref(idx);
    return ret;
}

static void deliver_fds(UMsg *m, VfsFile **fds_out, uint32_t cap, uint32_t *nout, int *mflags) {
    for (uint32_t i = 0; i < m->nfds; i++) {
        if (*nout < cap) fds_out[(*nout)++] = m->fds[i];
        else { kfile_close(m->fds[i]); *mflags |= UMSG_CTRUNC; }
        m->fds[i] = NULL;
    }
    m->nfds = 0;
}

static void consumed(USock *s, uint32_t bytes) {
    s->rx_bytes -= bytes;
    /* room to write again for whoever feeds us */
    if (s->peer) wake(s->peer);
    wake(s);
}

int64_t usock_recv(int idx, const UIoVecW *iov, int iovcnt, int flags, bool nonblock,
                   VfsFile **fds_out, uint32_t fds_cap, uint32_t *nfds_out, int *msg_flags,
                   char *src_name, uint32_t *src_len) {
    USock *s = slot(idx);
    if (!s) return -ENOTSOCK;
    if (flags & UMSG_DONTWAIT) nonblock = true;
    *nfds_out = 0;
    *msg_flags = 0;
    if (src_len) *src_len = 0;
    uint64_t want = 0;
    for (int i = 0; i < iovcnt; i++) want += iov[i].len;
    bool peek = (flags & UMSG_PEEK) != 0;

    s->refs++;
    int64_t ret;
    for (;;) {
        if (s->rx_head) break;
        if (s->type != USOCK_DGRAM) {
            if (s->rd_shut || s->peer_wr_shut) { ret = 0; goto out; }
            if (!s->peer) {
                if (s->was_connected) { ret = 0; goto out; }       /* EOF */
                if (s->state != US_CONNECTED) { ret = -ENOTCONN; goto out; }
            }
        }
        if (nonblock) { ret = -EAGAIN; goto out; }
        if (sched_dying()) { ret = -4; goto out; }   /* -EINTR */
        waitqueue_wait_ms(&s->wq, SCHED_NO_DEADLINE);
    }

    if (s->type == USOCK_STREAM) {
        uint64_t got = 0;
        int vi = 0; uint64_t voff = 0;
        UMsg *m = s->rx_head;
        uint32_t peek_off = 0;
        while (m && got < want) {
            if (m->nfds > 0 && got > 0) break; /* fds start a new read */
            if (m->nfds > 0 && !peek) deliver_fds(m, fds_out, fds_cap, nfds_out, msg_flags);
            uint32_t start = peek ? m->off + peek_off : m->off;
            uint32_t avail = m->len - start;
            while (avail > 0 && got < want) {
                while (vi < iovcnt && voff >= iov[vi].len) { vi++; voff = 0; }
                if (vi >= iovcnt) break;
                uint64_t take = iov[vi].len - voff;
                if (take > avail) take = avail;
                kmemcpy(iov[vi].base + voff, m->data + start, take);
                voff += take; got += take; start += (uint32_t)take; avail -= (uint32_t)take;
            }
            if (peek) {
                if (avail > 0) break;
                m = m->next; peek_off = 0;
                continue;
            }
            uint32_t used = start - m->off;
            m->off = start;
            if (used) consumed(s, used);
            if (m->off >= m->len) {
                s->rx_head = m->next;
                if (!s->rx_head) s->rx_tail = NULL;
                msg_free(m);
                dequeued(s);
                m = s->rx_head;
            } else {
                break;
            }
        }
        ret = (int64_t)got;
    } else {
        UMsg *m = s->rx_head;
        uint64_t n = m->len < want ? m->len : want;
        uint64_t done = 0;
        for (int i = 0; i < iovcnt && done < n; i++) {
            uint64_t take = iov[i].len;
            if (take > n - done) take = n - done;
            kmemcpy(iov[i].base, m->data + done, take);
            done += take;
        }
        if (m->len > want) *msg_flags |= UMSG_TRUNC;
        if (src_name && src_len && m->src_len) {
            kmemcpy(src_name, m->src, m->src_len);
            *src_len = m->src_len;
        }
        ret = (flags & UMSG_TRUNC) ? (int64_t)m->len : (int64_t)n;
        if (!peek) {
            deliver_fds(m, fds_out, fds_cap, nfds_out, msg_flags);
            s->rx_head = m->next;
            if (!s->rx_head) s->rx_tail = NULL;
            uint32_t len = m->len;
            msg_free(m);
            dequeued(s);
            consumed(s, len);
        }
    }
out:
    usock_unref(idx);
    return ret;
}

int64_t usock_shutdown(int idx, int how) {
    USock *s = slot(idx);
    if (!s) return -ENOTSOCK;
    if (how < 0 || how > 2) return -EINVAL;
    if (s->state != US_CONNECTED && s->type != USOCK_DGRAM) return -ENOTCONN;
    if (how == 0 || how == 2) s->rd_shut = true;
    if (how == 1 || how == 2) {
        s->wr_shut = true;
        if (s->peer && s->peer->peer == s) { s->peer->peer_wr_shut = true; wake(s->peer); }
    }
    wake(s);
    return 0;
}

int64_t usock_getname(int idx, bool peer, char *name, uint32_t *len) {
    USock *s = slot(idx);
    if (!s) return -ENOTSOCK;
    USock *t = s;
    if (peer) {
        t = s->peer;
        if (!t) return -ENOTCONN;
    }
    kmemcpy(name, t->name, t->name_len);
    *len = t->name_len;
    return 0;
}

int64_t usock_peercred(int idx, struct UCred *out) {
    USock *s = slot(idx);
    if (!s) return -ENOTSOCK;
    if (!s->was_connected && s->state != US_CONNECTED) {
        out->pid = 0; out->uid = (uint32_t)-1; out->gid = (uint32_t)-1;
        return 0;
    }
    *out = s->peer_cred;
    return 0;
}

int64_t usock_sock_error(int idx) {
    (void)idx;
    return 0;
}

uint32_t usock_poll(int idx, uint32_t events, WaitQueue **wq) {
    USock *s = slot(idx);
    if (!s) return UPOLLERR | UPOLLHUP;
    *wq = &s->wq;
    uint32_t rev = 0;
    if (s->state == US_LISTENING) {
        if (s->npend > 0) rev |= UPOLLIN;
        return rev & (events | UPOLLERR | UPOLLHUP);
    }
    if (s->rx_head) rev |= UPOLLIN;
    if (s->type != USOCK_DGRAM) {
        bool hup = (s->was_connected && !s->peer) || (s->rd_shut && s->wr_shut);
        if (hup) rev |= UPOLLHUP | UPOLLIN;
        if (s->peer_wr_shut || s->rd_shut) rev |= UPOLLRDHUP | UPOLLIN;
        if (s->peer && !s->wr_shut && s->peer->rx_bytes < USOCK_RCVBUF && s->peer->rx_msgs < USOCK_MAX_MSGS) rev |= UPOLLOUT;
        if (s->state == US_UNCONNECTED && !s->was_connected) rev |= UPOLLOUT | UPOLLHUP;
    } else {
        USock *r = s->peer;
        if (!r || (r->rx_bytes < USOCK_RCVBUF && r->rx_msgs < USOCK_MAX_MSGS)) rev |= UPOLLOUT;
    }
    return rev & (events | UPOLLERR | UPOLLHUP);
}
