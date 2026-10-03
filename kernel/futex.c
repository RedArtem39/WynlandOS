/*
 * WynlandOS - real futex support (Phase 22a, implementation)
 *
 * Blocking itself lives in the scheduler (sched_block/sched_unblock):
 * every thread owns a persistent kernel stack, so a futex wait is just
 * "park on a queue + schedule away", and resumption continues inside
 * sched_block()'s frame with its wake result in hand. This file only
 * implements the queue table keyed by (address space, uaddr) and the
 * Linux futex op semantics on top of it.
 *
 * Concurrency model: every entry point runs its check+enqueue+block
 * sequence and every wake path its walk+dequeue+wake sequence inside a
 * cli/sti window. There is no SMP here; interrupts-off IS the lock, the
 * same convention the rest of this kernel uses.
 */

#include <wynland/futex.h>
#include <wynland/types.h>
#include <wynland/heap.h>
#include <wynland/sched.h>
#include <wynland/irq.h>
#include <wynland/rtc.h>
#include <wynland/process.h>
#include <wynland/usercopy.h>

extern void serial_write_string(const char *str);

#define FUTEX_OP_MASK        (128u | 256u) /* PRIVATE_FLAG | CLOCK_REALTIME */
#define FUTEX_WAIT           0u
#define FUTEX_WAKE           1u
#define FUTEX_REQUEUE        3u
#define FUTEX_CMP_REQUEUE    4u
#define FUTEX_WAKE_OP        5u
#define FUTEX_WAIT_BITSET    9u
#define FUTEX_WAKE_BITSET   10u

/* One queue per (address space, futex word) that has waiters, made on the
   first wait and freed when it empties, in a hash table. There used to be
   a fixed table of 64 for the whole system: WebKit's thread pools plus
   the desktop's own ran it out, FUTEX_WAIT failed with ENOMEM and glibc
   aborted the process ("The futex facility returned an unexpected error
   code"). */
#define FUTEX_BUCKETS 1024

/* (named FutexQueue, not WaitQueue: process.h brings in the scheduler's
   WaitQueue -- this file only compiled while the stale object was reused) */
typedef struct FutexQueue {
    uint64_t key_pml4;
    uint64_t key_uaddr;
    Thread  *head;      /* intrusive FIFO via Thread.wq_next */
    struct FutexQueue *next;   /* bucket chain */
} FutexQueue;

static FutexQueue *g_buckets[FUTEX_BUCKETS];

void futex_init(void) {
    for (int i = 0; i < FUTEX_BUCKETS; i++) g_buckets[i] = NULL;
}

static unsigned bucket_of(uint64_t key_pml4, uint64_t key_uaddr) {
    uint64_t h = (key_uaddr >> 2) ^ ((key_pml4 >> 12) * 0x9E3779B97F4A7C15ULL);
    h ^= h >> 29;
    return (unsigned)(h & (FUTEX_BUCKETS - 1));
}

/* Prunes threads that stopped being BLOCKED out of the list while walking
   (a deadline pass can mark an entry READY without being able to unlink it
   -- the scheduler treats our queues as opaque). Returns the next live
   entry after `prev` semantics get messy; instead each helper re-walks. */
static void queue_prune(FutexQueue *q) {
    Thread **pp = &q->head;
    while (*pp) {
        if ((*pp)->state != THREAD_STATE_BLOCKED || (*pp)->wq != (void *)q) {
            Thread *stale = *pp;
            *pp = stale->wq_next; /* stale: already resumed some other way */
            stale->wq_next = NULL;
            stale->wq_head = NULL;
        } else {
            pp = &(*pp)->wq_next;
        }
    }
}

/* Callers hold interrupts off (the table is only touched that way). */
static FutexQueue *queue_lookup(uint64_t key_pml4, uint64_t key_uaddr, bool create) {
    unsigned b = bucket_of(key_pml4, key_uaddr);
    for (FutexQueue *q = g_buckets[b]; q; q = q->next)
        if (q->key_pml4 == key_pml4 && q->key_uaddr == key_uaddr) return q;
    if (!create) return NULL;
    FutexQueue *q = (FutexQueue *)kmalloc(sizeof(FutexQueue));
    if (!q) return NULL;
    q->key_pml4 = key_pml4;
    q->key_uaddr = key_uaddr;
    q->head = NULL;
    q->next = g_buckets[b];
    g_buckets[b] = q;
    return q;
}

/* An empty queue goes away. Nobody keeps a pointer to one across a sleep:
   a waiter finds its queue through Thread.wq_head (it keeps the queue
   non-empty while it is linked), see futex_unlink_self(). */
static void queue_release_if_empty(FutexQueue *q) {
    if (q->head != NULL) return;
    FutexQueue **pp = &g_buckets[bucket_of(q->key_pml4, q->key_uaddr)];
    while (*pp && *pp != q) pp = &(*pp)->next;
    if (*pp) *pp = q->next;
    kfree(q);
}

static void queue_push(FutexQueue *q, Thread *t) {
    t->wq_next = NULL;
    t->wq_head = &q->head;
    if (!q->head) {
        q->head = t;
        return;
    }
    Thread *tail = q->head;
    while (tail->wq_next) tail = tail->wq_next;
    tail->wq_next = t;
}

/* Unlinks `t` (the current thread, just resumed) from the queue it is in
   now, if any: wakers unlink (wq_head = NULL), a deadline pass leaves it
   linked, a requeue may have moved it to another queue. */
static void futex_unlink_self(Thread *t) {
    if (!t->wq_head) return;
    FutexQueue *q = (FutexQueue *)((uint8_t *)t->wq_head - __builtin_offsetof(FutexQueue, head));
    Thread **pp = &q->head;
    while (*pp) {
        if (*pp == t) {
            *pp = t->wq_next;
            t->wq_next = NULL;
            t->wq_head = NULL;
            break;
        }
        pp = &(*pp)->wq_next;
    }
    queue_release_if_empty(q);
}

static uint64_t current_key_pml4(void) {
    Thread *t = sched_current();
    return (t && t->proc) ? (uint64_t)(uintptr_t)t->proc->pml4 : 0;
}

/* Absolute-deadline conversion for FUTEX_WAIT_BITSET timeouts (Linux ABI:
   `timeout_arg` points to a struct timespec holding an ABSOLUTE time --
   CLOCK_REALTIME if the flag is set, CLOCK_MONOTONIC otherwise). Our timer
   gives 10ms ticks; REALTIME resolution is bounded by Phase 19's RTC
   reading (whole seconds), documented honestly rather than faked.

   All arithmetic here is SIGNED 64-bit: an absolute timeout earlier than
   our clock's epoch base (e.g. CLOCK_MONOTONIC minus 10s during the first
   seconds of uptime -- hit live by this phase's own regression test) must
   read as "already expired", not wrap into an astronomical future deadline
   once cast through unsigned math. */
static uint64_t abs_deadline_from_timespec(uint64_t ts_ptr, bool realtime) {
    struct { int64_t sec; int64_t nsec; } ts;
    memcpy(&ts, (const void *)ts_ptr, sizeof(ts));

    const int64_t NS_PER_TICK = 10000000LL; /* 100Hz */
    uint64_t now_ticks = timer_get_ticks();

    /* normalize to signed ns (tv_nsec is always 0..999999999 by POSIX,
       but don't trust it: fold overflows/underflows defensively) */
    int64_t target_ns = ts.sec * 1000000000LL + ts.nsec;
    int64_t now_ns;
    if (realtime) {
        now_ns = (int64_t)rtc_get_unix_time() * 1000000000LL;
    } else {
        now_ns = (int64_t)(now_ticks * (uint64_t)NS_PER_TICK);
    }

    if (target_ns <= now_ns) return now_ticks; /* already expired */
    int64_t delta_ns = target_ns - now_ns;
    return now_ticks + (uint64_t)(delta_ns / NS_PER_TICK) + 1;
}

int64_t futex_syscall(uint64_t uaddr, uint32_t op_raw, uint32_t val,
                      uint64_t timeout_arg, uint64_t uaddr2, uint32_t val3) {
    uint32_t op = op_raw & ~FUTEX_OP_MASK;
    bool realtime = (op_raw & 256u) != 0;
    uint64_t key = current_key_pml4();

    switch (op) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        volatile uint32_t *addr = (volatile uint32_t *)uaddr;

        uint64_t deadline = SCHED_NO_DEADLINE;
        if (op == FUTEX_WAIT_BITSET && timeout_arg != 0) {
            deadline = abs_deadline_from_timespec(timeout_arg, realtime);
        } else if (op == FUTEX_WAIT && timeout_arg != 0) {
            /* plain FUTEX_WAIT: a RELATIVE timespec. It used to be ignored,
               so every timed wait (QWaitCondition::wait(ms), QMutex::
               tryLock(ms), ...) could block forever. */
            struct { int64_t sec; int64_t nsec; } ts;
            memcpy(&ts, (const void *)timeout_arg, sizeof(ts));
            if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000LL) return -22; /* -EINVAL */
            uint64_t ms = (uint64_t)ts.sec * 1000 + (uint64_t)(ts.nsec + 999999) / 1000000;
            deadline = timer_get_ticks() + ms / 10 + 1; /* ticks are 10 ms */
        }

        /* The canonical futex contract: verify the caller's expected value
           atomically-with blocking. Under cli there is no window between
           this compare and parking ourselves on the queue. */
        uint64_t rflags;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

        if (*addr != val) {
            if (rflags & 0x200) __asm__ volatile("sti");
            return -11; /* -EAGAIN: value changed before we could sleep */
        }

        FutexQueue *q = queue_lookup(key, uaddr, true);
        if (!q) {
            if (rflags & 0x200) __asm__ volatile("sti");
            return -12; /* -ENOMEM: queue table exhausted */
        }

        Thread *self = sched_current();
        queue_push(q, self);
        int64_t r = sched_block((void *)q, deadline);
        /* Resumed: by a WAKE (already unlinked by it) or by the deadline
           pass (still linked, maybe requeued). Either way make sure we are
           off the list. `q` may be gone by now -- not used again. */
        __asm__ volatile("cli");
        futex_unlink_self(self);
        if (rflags & 0x200) __asm__ volatile("sti");
        return r;
    }

    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET: {
        /* WAIT_BITSET's uaddr2 carries a wake-bitset we deliberately ignore
           (wake any matcher): nothing on this OS uses bitset-selective
           wakes yet, documented v1 simplification. */
        uint64_t rflags;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));
        FutexQueue *q = queue_lookup(key, uaddr, false);
        uint32_t woken = 0;
        if (q) {
            queue_prune(q);
            while (q->head && woken < val) {
                Thread *t = q->head;
                q->head = t->wq_next;
                t->wq_next = NULL;
                t->wq_head = NULL;
                sched_unblock(t, 0);
                woken++;
            }
            queue_release_if_empty(q);
        }
        if (rflags & 0x200) __asm__ volatile("sti");
        return (int64_t)woken;
    }

    case FUTEX_WAKE_OP: {
        /* Atomically: old = *uaddr2; *uaddr2 = old OP oparg; wake up to
           val waiters on uaddr, and if (old CMP cmparg) up to val2 more on
           uaddr2. Qt's QSemaphore is built on it -- -ENOSYS left Qt's
           worker threads (the QML type loader) waiting forever. */
        uint32_t val2 = (uint32_t)timeout_arg;
        uint32_t opc = (val3 >> 28) & 0xF, cmp = (val3 >> 24) & 0xF;
        int32_t oparg = (int32_t)((val3 >> 12) & 0xFFF), cmparg = (int32_t)(val3 & 0xFFF);
        if (oparg & 0x800) oparg |= (int32_t)0xFFFFF000;   /* sign-extend 12 bits */
        if (cmparg & 0x800) cmparg |= (int32_t)0xFFFFF000;
        if (opc & 8) { opc &= 7; oparg = (int32_t)(1u << (oparg & 31)); } /* FUTEX_OP_OPARG_SHIFT */
        if ((uaddr2 & 3) || !user_prepare_write(uaddr2, 4)) return -14; /* -EFAULT */

        uint64_t rflags;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));
        volatile int32_t *a2p = (volatile int32_t *)uaddr2;
        int32_t old = *a2p, nv;
        switch (opc) {
        case 0: nv = oparg; break;           /* SET */
        case 1: nv = old + oparg; break;     /* ADD */
        case 2: nv = old | oparg; break;     /* OR */
        case 3: nv = old & ~oparg; break;    /* ANDN */
        case 4: nv = old ^ oparg; break;     /* XOR */
        default:
            if (rflags & 0x200) __asm__ volatile("sti");
            return -38;
        }
        *a2p = nv;
        bool cond;
        switch (cmp) {
        case 0: cond = old == cmparg; break;
        case 1: cond = old != cmparg; break;
        case 2: cond = old <  cmparg; break;
        case 3: cond = old <= cmparg; break;
        case 4: cond = old >  cmparg; break;
        case 5: cond = old >= cmparg; break;
        default: cond = false; break;
        }
        uint32_t woken = 0;
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1 && !cond) break;
            FutexQueue *q = queue_lookup(key, pass ? uaddr2 : uaddr, false);
            uint32_t limit = pass ? val2 : val, n = 0;
            if (!q) continue;
            queue_prune(q);
            while (q->head && n < limit) {
                Thread *t = q->head;
                q->head = t->wq_next;
                t->wq_next = NULL;
                t->wq_head = NULL;
                sched_unblock(t, 0);
                n++;
            }
            woken += n;
            queue_release_if_empty(q);
        }
        if (rflags & 0x200) __asm__ volatile("sti");
        return (int64_t)woken;
    }

    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE: {
        volatile uint32_t *addr = (volatile uint32_t *)uaddr;
        uint32_t nr_requeue = (uint32_t)timeout_arg; /* arg4 = val2 */
        uint64_t uaddr2_key_addr = uaddr2;

        uint64_t rflags;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

        /* plain REQUEUE has no expected-value check */
        if (op == FUTEX_CMP_REQUEUE && *addr != val3) {
            if (rflags & 0x200) __asm__ volatile("sti");
            return -11; /* -EAGAIN */
        }

        FutexQueue *q = queue_lookup(key, uaddr, false);
        if (!q) {
            if (rflags & 0x200) __asm__ volatile("sti");
            return 0;
        }
        queue_prune(q);

        uint32_t woken = 0;
        while (q->head && woken < val) {
            Thread *t = q->head;
            q->head = t->wq_next;
            t->wq_next = NULL;
            t->wq_head = NULL;
            sched_unblock(t, 0);
            woken++;
        }

        uint32_t moved = 0;
        if (nr_requeue > 0 && q->head && uaddr2_key_addr != uaddr) {
            FutexQueue *q2 = queue_lookup(key, uaddr2_key_addr, true);
            if (q2) {
                /* splice up to nr_requeue remaining waiters over */
                Thread **src = &q->head;
                while (*src && moved < nr_requeue) {
                    Thread *t = *src;
                    *src = t->wq_next;
                    queue_push(q2, t);
                    t->wq = (void *)q2;   /* or q2's prune drops it as stale: a lost wakeup */
                    moved++;
                }
                queue_release_if_empty(q2);
            }
        }
        queue_release_if_empty(q);

        if (rflags & 0x200) __asm__ volatile("sti");
        return (int64_t)(woken + moved);
    }

    default:
        /* LOCK_PI/WAKE_OP/etc: nothing uses them yet; fail loudly rather
           than pretend success (that pretense was bug #1 in this file's
           history). */
        return -38; /* -ENOSYS */
    }
}

/* Kernel-internal wake used by thread death (CLONE_CHILD_CLEARTID): wakes
   up to `n` waiters parked on (current address space, uaddr). Runs in the
   DYING thread's context -- its CR3 and Process are still valid, which is
   exactly what the keying needs. Interrupt state handled internally. */
void futex_wake_user(uint64_t uaddr, uint32_t n) {
    uint64_t key = current_key_pml4();
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));
    FutexQueue *q = queue_lookup(key, uaddr, false);
    uint32_t woken = 0;
    if (q) {
        queue_prune(q);
        while (q->head && woken < n) {
            Thread *t = q->head;
            q->head = t->wq_next;
            t->wq_next = NULL;
            t->wq_head = NULL;
            sched_unblock(t, 0);
            woken++;
        }
        queue_release_if_empty(q);
    }
    if (rflags & 0x200) __asm__ volatile("sti");
}
