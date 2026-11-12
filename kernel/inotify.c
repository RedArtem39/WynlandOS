/*
 * WynlandOS - inotify: what happens to files and directories, told.
 *
 * An inotify fd (INOTIFY_FD, include/wynland/kfile.h) holds watches -- an
 * inode and the events wanted on it -- and a queue of struct
 * inotify_event records, read() whole (EINVAL when the first does not
 * fit), poll()ed readable while it is not empty. The filesystem tells it
 * what happened (drivers/fs/ext2.c, inotify_note()): an event goes to the
 * watches on the inode itself (no name) and on its directory (with the
 * name), as Linux's. A queue that is full gets one IN_Q_OVERFLOW; the same
 * event again on the end of the queue is merged, as Linux does.
 *
 * Node's fs.watch, dev servers' reloads, file managers, Qt's and GLib's
 * file monitors, editors noticing a file changed on disk: they all sit on
 * it (dbus said "Cannot initialize inotify" at every start).
 *
 * Copyright (C) 2026 Red_Artem39. GPL-2.0-or-later.
 */
#include <wynland/types.h>
#include <wynland/heap.h>
#include <wynland/spinlock.h>
#include <wynland/inotify.h>
#include <wynland/waitqueue.h>

extern WaitQueue g_poll_any_wq;   /* poll()/epoll_wait() re-check on it */
static void inotify_wake(void) { waitqueue_wake_all(&g_poll_any_wq); }

static int memcmp_k(const void *a, const void *b, uint32_t n) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (uint32_t k = 0; k < n; k++) if (x[k] != y[k]) return x[k] - y[k];
    return 0;
}

static volatile uint32_t g_cookie;
uint32_t inotify_cookie(void) { return __atomic_add_fetch(&g_cookie, 1, __ATOMIC_RELAXED) ?: 1; }

#define IN_SLOTS    64
#define IN_WATCHES  1024       /* each instance (a dev server watches every directory of a project) */
#define IN_QUEUE    (16 * 1024)

#define IN_Q_OVERFLOW 0x00004000u
#define IN_IGNORED    0x00008000u
#define IN_ONESHOT    0x80000000u
#define IN_MASK_ADD   0x20000000u
#define IN_ALL_EVENTS 0x00000fffu

typedef struct { int32_t wd; uint32_t inum; uint32_t mask; } Watch;

typedef struct {
    uint32_t refs;
    Watch   *w;            /* IN_WATCHES of them; inum 0: free */
    int32_t  next_wd;
    uint8_t *q;            /* IN_QUEUE bytes of events */
    uint32_t qlen;
    uint32_t last;         /* offset of the last event (merging) */
    uint32_t hw;           /* watches used are below this index */
    bool     overflowed;
} Inst;

static Inst g_in[IN_SLOTS];
static Spinlock g_in_lock = SPINLOCK_INIT("inotify");
static volatile uint32_t g_in_watches;   /* watches anywhere: 0 -> the filesystem skips all this */

bool inotify_active(void) { return __atomic_load_n(&g_in_watches, __ATOMIC_RELAXED) != 0; }

/* the watched inodes, a bit each (inum mod 4096): a write to a file no
   one watches, nor its directory, is turned away before the lock and
   the walk over every watch. Set on add, rebuilt when watches end. */
static uint64_t g_bloom[64];
static inline bool bloom_has(uint32_t inum) {
    return inum && (__atomic_load_n(&g_bloom[(inum >> 6) & 63], __ATOMIC_RELAXED) >> (inum & 63) & 1);
}
static void bloom_rebuild(void) {   /* g_in_lock held */
    uint64_t b[64];
    memset(b, 0, sizeof b);
    for (int i = 0; i < IN_SLOTS; i++) {
        if (!g_in[i].refs) continue;
        for (uint32_t k = 0; k < g_in[i].hw; k++) {
            uint32_t n = g_in[i].w[k].inum;
            if (n) b[(n >> 6) & 63] |= 1ull << (n & 63);
        }
    }
    for (int j = 0; j < 64; j++) __atomic_store_n(&g_bloom[j], b[j], __ATOMIC_RELAXED);
}

int inotify_create(void) {
    uint64_t f = spin_lock_irqsave(&g_in_lock);
    for (int i = 0; i < IN_SLOTS; i++) {
        if (g_in[i].refs) continue;
        if (!g_in[i].w) {
            spin_unlock_irqrestore(&g_in_lock, f);
            Watch *w = (Watch *)kmalloc(sizeof(Watch) * IN_WATCHES);
            uint8_t *q = (uint8_t *)kmalloc(IN_QUEUE);
            if (!w || !q) { if (w) kfree(w); if (q) kfree(q); return -12; }
            f = spin_lock_irqsave(&g_in_lock);
            if (g_in[i].refs || g_in[i].w) {          /* (taken meanwhile) */
                spin_unlock_irqrestore(&g_in_lock, f);
                kfree(w); kfree(q);
                return inotify_create();
            }
            g_in[i].w = w; g_in[i].q = q;
        }
        memset(g_in[i].w, 0, sizeof(Watch) * IN_WATCHES);
        g_in[i].refs = 1;
        g_in[i].next_wd = 1;
        g_in[i].qlen = g_in[i].last = g_in[i].hw = 0;
        g_in[i].overflowed = false;
        spin_unlock_irqrestore(&g_in_lock, f);
        return i;
    }
    spin_unlock_irqrestore(&g_in_lock, f);
    return -24;   /* -EMFILE */
}

void inotify_ref(int i) {
    if (i < 0 || i >= IN_SLOTS) return;
    uint64_t f = spin_lock_irqsave(&g_in_lock);
    g_in[i].refs++;
    spin_unlock_irqrestore(&g_in_lock, f);
}

void inotify_unref(int i) {
    if (i < 0 || i >= IN_SLOTS) return;
    uint64_t f = spin_lock_irqsave(&g_in_lock);
    if (g_in[i].refs && --g_in[i].refs == 0) {
        for (uint32_t k = 0; k < g_in[i].hw; k++)
            if (g_in[i].w[k].inum) { g_in[i].w[k].inum = 0; g_in_watches--; }
        g_in[i].qlen = g_in[i].hw = 0;
        bloom_rebuild();
    }
    spin_unlock_irqrestore(&g_in_lock, f);
}

bool inotify_readable(int i) {
    return i >= 0 && i < IN_SLOTS && g_in[i].refs && g_in[i].qlen > 0;
}

uint32_t inotify_pending(int i) {
    return (i >= 0 && i < IN_SLOTS && g_in[i].refs) ? g_in[i].qlen : 0;
}

static uint32_t name_len(const char *s) { uint32_t n = 0; while (s && s[n]) n++; return n; }

/* g_in_lock held: one event onto an instance's queue */
static void queue(Inst *in, int32_t wd, uint32_t mask, uint32_t cookie, const char *name) {
    uint32_t nl = name_len(name);
    uint32_t pad = nl ? (nl + 1 + 15) & ~15u : 0;   /* the name NUL-padded, as Linux's */
    uint32_t sz = 16 + pad;
    /* the same as the last one (and nothing between): merged */
    if (in->qlen && !cookie) {
        uint8_t *l = in->q + in->last;
        int32_t lwd; uint32_t lmask, lcookie, llen;
        memcpy(&lwd, l, 4); memcpy(&lmask, l + 4, 4); memcpy(&lcookie, l + 8, 4); memcpy(&llen, l + 12, 4);
        if (lwd == wd && lmask == mask && !lcookie && llen == pad &&
            (!nl || (l[16 + nl] == 0 && !memcmp_k(l + 16, name, nl))))
            return;
    }
    if (in->qlen + sz > IN_QUEUE - 16) {
        if (!in->overflowed) {                       /* one IN_Q_OVERFLOW, wd -1 */
            uint8_t *o = in->q + in->qlen;
            int32_t m1 = -1; uint32_t ov = IN_Q_OVERFLOW, z = 0;
            memcpy(o, &m1, 4); memcpy(o + 4, &ov, 4); memcpy(o + 8, &z, 4); memcpy(o + 12, &z, 4);
            in->last = in->qlen;
            in->qlen += 16;
            in->overflowed = true;
        }
        return;
    }
    uint8_t *e = in->q + in->qlen;
    memcpy(e, &wd, 4); memcpy(e + 4, &mask, 4); memcpy(e + 8, &cookie, 4); memcpy(e + 12, &pad, 4);
    if (pad) {
        memset(e + 16, 0, pad);
        memcpy(e + 16, name, nl);
    }
    in->last = in->qlen;
    in->qlen += sz;
}

int32_t inotify_add(int i, uint32_t inum, uint32_t mask) {
    if (i < 0 || i >= IN_SLOTS || !inum) return -22;
    if (!(mask & IN_ALL_EVENTS)) return -22;
    uint64_t f = spin_lock_irqsave(&g_in_lock);
    Inst *in = &g_in[i];
    int32_t wd = -28;   /* -ENOSPC: out of watches */
    for (uint32_t k = 0; k < in->hw; k++) {
        if (in->w[k].inum == inum) {                 /* watched already: its mask, the same wd */
            if (mask & 0x10000000u /* IN_MASK_CREATE */) { spin_unlock_irqrestore(&g_in_lock, f); return -17; }
            in->w[k].mask = (mask & IN_MASK_ADD) ? (in->w[k].mask | mask) : mask;
            in->w[k].mask &= ~IN_MASK_ADD;
            wd = in->w[k].wd;
            spin_unlock_irqrestore(&g_in_lock, f);
            return wd;
        }
    }
    for (int k = 0; k < IN_WATCHES; k++) {
        if (in->w[k].inum) continue;
        in->w[k].inum = inum;
        in->w[k].mask = mask & ~IN_MASK_ADD;
        in->w[k].wd = in->next_wd++;
        wd = in->w[k].wd;
        if ((uint32_t)k + 1 > in->hw) in->hw = (uint32_t)k + 1;
        __atomic_or_fetch(&g_bloom[(inum >> 6) & 63], 1ull << (inum & 63), __ATOMIC_RELAXED);
        g_in_watches++;
        break;
    }
    spin_unlock_irqrestore(&g_in_lock, f);
    return wd;
}

int inotify_rm(int i, int32_t wd) {
    if (i < 0 || i >= IN_SLOTS) return -22;
    uint64_t f = spin_lock_irqsave(&g_in_lock);
    Inst *in = &g_in[i];
    for (uint32_t k = 0; k < in->hw; k++) {
        if (in->w[k].inum && in->w[k].wd == wd) {
            in->w[k].inum = 0;
            g_in_watches--;
            bloom_rebuild();
            queue(in, wd, IN_IGNORED, 0, NULL);
            spin_unlock_irqrestore(&g_in_lock, f);
            inotify_wake();
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_in_lock, f);
    return -22;   /* -EINVAL: no such watch */
}

/* What happened: `mask` (IN_* event bits, IN_ISDIR for a directory) to
   the inode `inum` named `name` in the directory `dir`. Watches on the
   inode get it without a name, watches on the directory with it. An
   inode gone (DELETE_SELF) loses its watches: IN_IGNORED follows. */
void inotify_note(uint32_t dir, uint32_t inum, uint32_t mask, uint32_t cookie, const char *name) {
    if (!inotify_active() || !(bloom_has(inum) || bloom_has(dir))) return;
    bool any = false, gone = false;
    uint64_t f = spin_lock_irqsave(&g_in_lock);
    for (int i = 0; i < IN_SLOTS; i++) {
        Inst *in = &g_in[i];
        if (!in->refs) continue;
        for (uint32_t k = 0; k < in->hw; k++) {
            Watch *w = &in->w[k];
            if (!w->inum) continue;
            uint32_t ev = mask & IN_ALL_EVENTS;
            if (!(w->mask & ev)) continue;
            /* CREATE/DELETE/MOVED_* are the directory's events, *_SELF the inode's */
            if (inum && w->inum == inum && !(ev & (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)))
                queue(in, w->wd, mask, cookie, NULL);
            else if (dir && w->inum == dir && !(ev & (IN_DELETE_SELF | IN_MOVE_SELF)))
                queue(in, w->wd, mask, cookie, name);
            else continue;
            any = true;
            if (w->mask & IN_ONESHOT) {
                w->inum = 0;
                g_in_watches--;
                gone = true;
                queue(in, w->wd, IN_IGNORED, 0, NULL);
            }
        }
        /* the inode is gone: its watches end */
        if (inum && (mask & IN_DELETE_SELF)) {
            for (uint32_t k = 0; k < in->hw; k++) {
                Watch *w = &in->w[k];
                if (w->inum != inum) continue;
                w->inum = 0;
                g_in_watches--;
                gone = true;
                queue(in, w->wd, IN_IGNORED, 0, NULL);
                any = true;
            }
        }
    }
    if (gone) bloom_rebuild();
    spin_unlock_irqrestore(&g_in_lock, f);
    if (any) {
        inotify_wake();
    }
}

/* the waiting events into out (cap bytes): whole events only. 0 when
   none; -EINVAL when the first does not fit */
int64_t inotify_read(int i, uint8_t *out, uint32_t cap) {
    if (i < 0 || i >= IN_SLOTS) return -9;
    uint64_t f = spin_lock_irqsave(&g_in_lock);
    Inst *in = &g_in[i];
    uint32_t n = 0;
    while (n < in->qlen) {
        uint32_t len;
        memcpy(&len, in->q + n + 12, 4);
        if (n + 16 + len > cap) break;
        n += 16 + len;
    }
    if (n == 0 && in->qlen) { spin_unlock_irqrestore(&g_in_lock, f); return -22; }
    memcpy(out, in->q, n);
    for (uint32_t k = n; k < in->qlen; k++) in->q[k - n] = in->q[k];
    in->qlen -= n;
    in->last = in->last >= n ? in->last - n : 0;
    if (in->qlen < IN_QUEUE / 2) in->overflowed = false;
    spin_unlock_irqrestore(&g_in_lock, f);
    return n;
}
