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
 * Concurrency: the queue table is under g_futex_lock, not the big kernel
 * lock -- futex() runs outside it (kernel/syscall.c syscall_lock()). A wait
 * checks the word and parks itself with the lock held, and lets go only
 * once it is BLOCKED (sched_block_unlock()), so a wake between the two is
 * never lost. The word is read without faulting under the lock (a page
 * fault may need the big lock, or sleep); when that fails the lock goes,
 * the word is touched normally, and the wait starts over. Keying a
 * non-private futex reads page tables: that takes the big lock.
 */

#include <wynland/futex.h>
#include <wynland/types.h>
#include <wynland/heap.h>
#include <wynland/sched.h>
#include <wynland/irq.h>
#include <wynland/rtc.h>
#include <wynland/process.h>
#include <wynland/usercopy.h>
#include <wynland/signal.h>
#include <wynland/spinlock.h>
#include <wynland/smp.h>

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
static Spinlock g_futex_lock = SPINLOCK_INIT("futex");

/* The queues come from a pool, not the kernel heap (its allocator is the
   big lock's): one per futex word that has waiters right now */
#define FUTEX_QUEUES 8192
static FutexQueue g_pool[FUTEX_QUEUES];
static FutexQueue *g_pool_free;

static bool g_pool_ready;

/* the pool's free list, on first use (g_futex_lock held) */
static void pool_init(void) {
    g_pool_free = NULL;
    for (int i = FUTEX_QUEUES - 1; i >= 0; i--) {
        g_pool[i].next = g_pool_free;
        g_pool_free = &g_pool[i];
    }
    g_pool_ready = true;
}

void futex_init(void) {
    /* statics start zeroed: nothing to do (the pool fills on first use) */
}

uint64_t futex_lock_irqsave(void) { return spin_lock_irqsave(&g_futex_lock); }
void futex_unlock_irqrestore(uint64_t f) { spin_unlock_irqrestore(&g_futex_lock, f); }

extern int uaccess_read32_nofault(const uint32_t *uaddr, uint32_t *out);   /* kernel/uaccess.asm */

/* The futex word, with g_futex_lock held: 0, or -EFAULT when it cannot be
   read without a page fault (or is no user address at all) */
static int word_nofault(uint64_t uaddr, uint32_t *v) {
    if (uaddr >= 0x0000800000000000ULL - 4) return -14;
    return uaccess_read32_nofault((const uint32_t *)uaddr, v);
}

/* g_futex_lock, and the word under it. A word that is not there yet (a
   lazy page, swapped-out never happens here): the lock goes, the word is
   faulted in by a normal copy, and again. -EFAULT for a bad address. */
static int lock_and_read(uint64_t uaddr, uint32_t *v, uint64_t *flags) {
    for (int tries = 0; tries < 8; tries++) {
        *flags = spin_lock_irqsave(&g_futex_lock);
        if (word_nofault(uaddr, v) == 0) return 0;
        spin_unlock_irqrestore(&g_futex_lock, *flags);
        uint32_t probe;
        if (copy_from_user(&probe, (const void *)uaddr, sizeof probe) != 0) return -14;
    }
    return -14;
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

/* Callers hold g_futex_lock. */
static FutexQueue *queue_lookup(uint64_t key_pml4, uint64_t key_uaddr, bool create) {
    unsigned b = bucket_of(key_pml4, key_uaddr);
    for (FutexQueue *q = g_buckets[b]; q; q = q->next)
        if (q->key_pml4 == key_pml4 && q->key_uaddr == key_uaddr) return q;
    if (!create) return NULL;
    if (!g_pool_ready) pool_init();
    FutexQueue *q = g_pool_free;
    if (!q) return NULL;
    g_pool_free = q->next;
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
    q->next = g_pool_free;
    g_pool_free = q;
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

/* The queue key of a futex word: (address space, address), unless the
   word is in memory shared between processes (PAGE_SHARED_MAP: a
   MAP_SHARED file or memfd page, SysV-style SHM) and the caller did not
   say FUTEX_PRIVATE_FLAG -- then its physical address, the same in every
   process that maps it (key_pml4 1, never a page table's address). A
   process-shared semaphore (Python's multiprocessing: sem_open() in
   /dev/shm) waited on in one process and posted in another met in no
   queue before. */
#define FUTEX_KEY_SHARED 1
static void futex_key(uint64_t uaddr, bool priv, uint64_t *kp, uint64_t *ka) {
    *kp = current_key_pml4();
    *ka = uaddr;
    Thread *t = sched_current();
    if (priv || !t || !t->proc || !t->proc->pml4) return;
    bkl_lock();   /* the page tables are the big lock's (held until the syscall ends) */
    uint64_t pte = vmm_get_pte(t->proc->pml4, uaddr & ~0xFFFULL);
    if (!(pte & PAGE_PRESENT)) {                 /* not touched yet: fault it in */
        uint32_t probe;                          /* a bad address just stays unkeyed */
        if (copy_from_user(&probe, (const void *)uaddr, sizeof probe) != 0) return;
        pte = vmm_get_pte(t->proc->pml4, uaddr & ~0xFFFULL);
    }
    if ((pte & PAGE_PRESENT) && (pte & PAGE_SHARED_MAP)) {
        *kp = FUTEX_KEY_SHARED;
        *ka = (pte & 0x000FFFFFFFFFF000ULL) | (uaddr & 0xFFFULL);
    }
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
/* The deadline on the 1 kHz clock (ms). The caller's time came from
   clock_gettime -- the TSC clock (kernel/sched.c) or the wall clock --
   so what is left is measured on that same clock; comparing it with the
   timer's own count put any difference between the two onto every timed
   wait (Qt's and glibc's condition variables). */
static bool abs_deadline_from_timespec(uint64_t ts_ptr, bool realtime, uint64_t *out_ms) {
    struct { int64_t sec; int64_t nsec; } ts;
    if (copy_from_user(&ts, (const void *)ts_ptr, sizeof(ts)) != 0) return false;
    extern uint64_t sched_clock_ns(void);
    extern uint64_t rtc_get_unix_time_ns(void);
    extern uint64_t timer_get_ms(void);

    /* normalize to signed ns (tv_nsec is always 0..999999999 by POSIX,
       but don't trust it: fold overflows/underflows defensively) */
    int64_t target_ns = ts.sec * 1000000000LL + ts.nsec;
    int64_t now_ns = realtime ? (int64_t)rtc_get_unix_time_ns() : (int64_t)sched_clock_ns();
    uint64_t now_ms = timer_get_ms();

    if (target_ns <= now_ns) { *out_ms = now_ms; return true; } /* already expired */
    uint64_t delta_ns = (uint64_t)(target_ns - now_ns);
    *out_ms = now_ms + (delta_ns + 999999) / 1000000;
    return true;
}

/* A signal that would do something (run a handler, kill) waits for the
   calling thread. Raised from another core between the caller's look at
   its word and this call (a child's SIGCHLD, whose handler posts the
   semaphore it is about to wait on -- fish): sleeping now would never end,
   nothing else wakes it. Linux's futex_wait returns -EINTR then. */
static bool wake_signal_pending(void) {
    Thread *t = sched_current();
    uint64_t p = t ? t->sig_pending : 0;
    for (int sig = 1; p && sig < 65; sig++)
        if ((p & (1ULL << sig)) && signal_wants_wake(t, sig)) return true;
    return false;
}

int64_t futex_syscall(uint64_t uaddr, uint32_t op_raw, uint32_t val,
                      uint64_t timeout_arg, uint64_t uaddr2, uint32_t val3) {
    uint32_t op = op_raw & ~FUTEX_OP_MASK;
    bool realtime = (op_raw & 256u) != 0;
    const bool priv = (op_raw & 128u) != 0;
    if (uaddr & 3) return -22; /* -EINVAL: futex words are aligned */
    uint64_t key, kaddr;
    futex_key(uaddr, priv, &key, &kaddr);

    switch (op) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        uint64_t deadline = SCHED_NO_DEADLINE;
        if (op == FUTEX_WAIT_BITSET && timeout_arg != 0) {
            if (!abs_deadline_from_timespec(timeout_arg, realtime, &deadline)) return -14; /* -EFAULT */
        } else if (op == FUTEX_WAIT && timeout_arg != 0) {
            /* plain FUTEX_WAIT: a RELATIVE timespec. It used to be ignored,
               so every timed wait (QWaitCondition::wait(ms), QMutex::
               tryLock(ms), ...) could block forever. */
            struct { int64_t sec; int64_t nsec; } ts;
            if (copy_from_user(&ts, (const void *)timeout_arg, sizeof(ts)) != 0) return -14; /* -EFAULT */
            if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000LL) return -22; /* -EINVAL */
            uint64_t ms = (uint64_t)ts.sec * 1000 + (uint64_t)(ts.nsec + 999999) / 1000000;
            extern uint64_t timer_get_ms(void);
            deadline = timer_get_ms() + ms;   /* to the millisecond (it was 10 ms ticks: a 1 ms wait took 10) */
        }

        /* The canonical futex contract: verify the caller's expected value
           atomically-with blocking. Under g_futex_lock there is no window
           between this compare and parking ourselves on the queue (a waker
           needs the lock to find us). */
        uint64_t rflags;
        uint32_t cur;
        if (lock_and_read(uaddr, &cur, &rflags) != 0) return -14; /* -EFAULT */

        if (cur != val) {
            spin_unlock_irqrestore(&g_futex_lock, rflags);
            return -11; /* -EAGAIN: value changed before we could sleep */
        }
        if (wake_signal_pending()) {
            spin_unlock_irqrestore(&g_futex_lock, rflags);
            return -4;  /* -EINTR */
        }

        FutexQueue *q = queue_lookup(key, kaddr, true);
        if (!q) {
            spin_unlock_irqrestore(&g_futex_lock, rflags);
            return -12; /* -ENOMEM: queue table exhausted */
        }

        Thread *self = sched_current();
        queue_push(q, self);
        int64_t r = sched_block_unlock_ex((void *)q, SCHED_NO_DEADLINE, deadline, &g_futex_lock, true);
        /* Resumed: by a WAKE (already unlinked by it) or by the deadline
           pass (still linked, maybe requeued) -- or it never slept (a
           signal). Either way make sure we are off the list. `q` may be
           gone by now -- not used again. */
        spin_lock(&g_futex_lock);
        futex_unlink_self(self);
        spin_unlock_irqrestore(&g_futex_lock, rflags);
        return r;
    }

    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET: {
        /* WAIT_BITSET's uaddr2 carries a wake-bitset we deliberately ignore
           (wake any matcher): nothing on this OS uses bitset-selective
           wakes yet, documented v1 simplification. */
        uint64_t rflags = spin_lock_irqsave(&g_futex_lock);
        FutexQueue *q = queue_lookup(key, kaddr, false);
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
        spin_unlock_irqrestore(&g_futex_lock, rflags);
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
        /* (user_prepare_write() takes the big lock: the page stays mapped
           and writable while it is held) */
        if ((uaddr2 & 3) || !user_prepare_write(uaddr2, 4)) return -14; /* -EFAULT */
        if (opc > 4) return -38;
        uint64_t key2, kaddr2;
        futex_key(uaddr2, priv, &key2, &kaddr2);

        uint64_t rflags = spin_lock_irqsave(&g_futex_lock);
        /* A real atomic update (lock cmpxchg): the process's threads on
           other cores change this word with atomics of their own at the
           same time -- a plain load and store lost their update (a
           semaphore post, a condvar sequence), and a waiter slept on. */
        int32_t *a2p = (int32_t *)uaddr2;
        int32_t old = __atomic_load_n(a2p, __ATOMIC_RELAXED), nv;
        do {
            switch (opc) {
            case 0:  nv = oparg; break;           /* SET */
            case 1:  nv = old + oparg; break;     /* ADD */
            case 2:  nv = old | oparg; break;     /* OR */
            case 3:  nv = old & ~oparg; break;    /* ANDN */
            default: nv = old ^ oparg; break;     /* XOR */
            }
        } while (!__atomic_compare_exchange_n(a2p, &old, nv, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
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
            FutexQueue *q = pass ? queue_lookup(key2, kaddr2, false) : queue_lookup(key, kaddr, false);
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
        spin_unlock_irqrestore(&g_futex_lock, rflags);
        return (int64_t)woken;
    }

    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE: {
        uint32_t nr_requeue = (uint32_t)timeout_arg; /* arg4 = val2 */
        uint64_t key2, uaddr2_key_addr;
        if (uaddr2 & 3) return -22;
        futex_key(uaddr2, priv, &key2, &uaddr2_key_addr);

        uint64_t rflags;
        if (op == FUTEX_CMP_REQUEUE) {
            uint32_t cur;
            if (lock_and_read(uaddr, &cur, &rflags) != 0) return -14; /* -EFAULT */
            if (cur != val3) {
                spin_unlock_irqrestore(&g_futex_lock, rflags);
                return -11; /* -EAGAIN */
            }
        } else {
            rflags = spin_lock_irqsave(&g_futex_lock);   /* plain REQUEUE has no expected-value check */
        }

        FutexQueue *q = queue_lookup(key, kaddr, false);
        if (!q) {
            spin_unlock_irqrestore(&g_futex_lock, rflags);
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
        if (nr_requeue > 0 && q->head && (key2 != key || uaddr2_key_addr != kaddr)) {
            FutexQueue *q2 = queue_lookup(key2, uaddr2_key_addr, true);
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

        spin_unlock_irqrestore(&g_futex_lock, rflags);
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
    uint64_t key, kaddr;
    futex_key(uaddr, false, &key, &kaddr);   /* as Linux: a shared key where the memory is shared */
    uint64_t rflags = spin_lock_irqsave(&g_futex_lock);
    FutexQueue *q = queue_lookup(key, kaddr, false);
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
    spin_unlock_irqrestore(&g_futex_lock, rflags);
}
