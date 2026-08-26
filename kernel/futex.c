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

extern void serial_write_string(const char *str);

#define FUTEX_OP_MASK        (128u | 256u) /* PRIVATE_FLAG | CLOCK_REALTIME */
#define FUTEX_WAIT           0u
#define FUTEX_WAKE           1u
#define FUTEX_CMP_REQUEUE    4u
#define FUTEX_WAIT_BITSET    9u
#define FUTEX_WAKE_BITSET   10u

#define MAX_FUTEX_QUEUES 64

typedef struct WaitQueue {
    uint64_t key_pml4;
    uint64_t key_uaddr;
    Thread  *head;      /* intrusive FIFO via Thread.wq_next */
    bool     in_use;
} WaitQueue;

static WaitQueue g_queues[MAX_FUTEX_QUEUES];

void futex_init(void) {
    for (int i = 0; i < MAX_FUTEX_QUEUES; i++) {
        g_queues[i].in_use = false;
        g_queues[i].head = NULL;
    }
}

/* Prunes threads that stopped being BLOCKED out of the list while walking
   (a deadline pass can mark an entry READY without being able to unlink it
   -- the scheduler treats our queues as opaque). Returns the next live
   entry after `prev` semantics get messy; instead each helper re-walks. */
static void queue_prune(WaitQueue *q) {
    Thread **pp = &q->head;
    while (*pp) {
        if ((*pp)->state != THREAD_STATE_BLOCKED || (*pp)->wq != (void *)q) {
            *pp = (*pp)->wq_next; /* stale: already resumed some other way */
        } else {
            pp = &(*pp)->wq_next;
        }
    }
}

static WaitQueue *queue_lookup(uint64_t key_pml4, uint64_t key_uaddr, bool create) {
    WaitQueue *free_slot = NULL;
    for (int i = 0; i < MAX_FUTEX_QUEUES; i++) {
        WaitQueue *q = &g_queues[i];
        if (q->in_use && q->key_pml4 == key_pml4 && q->key_uaddr == key_uaddr) {
            return q;
        }
        if (!q->in_use && !free_slot) free_slot = q;
    }
    if (!create || !free_slot) return NULL;
    free_slot->in_use = true;
    free_slot->key_pml4 = key_pml4;
    free_slot->key_uaddr = key_uaddr;
    free_slot->head = NULL;
    return free_slot;
}

static void queue_release_if_empty(WaitQueue *q) {
    if (q->head == NULL) q->in_use = false;
}

static void queue_push(WaitQueue *q, Thread *t) {
    t->wq_next = NULL;
    if (!q->head) {
        q->head = t;
        return;
    }
    Thread *tail = q->head;
    while (tail->wq_next) tail = tail->wq_next;
    tail->wq_next = t;
}

/* Unlinks `t` (the current thread, just resumed) from its queue. Called by
   the waiter itself after sched_block() returns -- covers both wake paths,
   since wakers may have left the link in place when they found the thread
   already unblocked via deadline. Self-service keeps every path correct
   without the scheduler knowing queue internals. */
static void self_unlink(WaitQueue *q, Thread *t) {
    Thread **pp = &q->head;
    while (*pp) {
        if (*pp == t) {
            *pp = t->wq_next;
            t->wq_next = NULL;
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

        WaitQueue *q = queue_lookup(key, uaddr, true);
        if (!q) {
            if (rflags & 0x200) __asm__ volatile("sti");
            return -12; /* -ENOMEM: queue table exhausted */
        }

        Thread *self = sched_current();
        queue_push(q, self);
        int64_t r = sched_block((void *)q, deadline);
        /* Resumed: by a WAKE (already unlinked by it) or by the deadline
           pass (still linked). Either way make sure we are off the list. */
        __asm__ volatile("cli");
        self_unlink(q, self);
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
        WaitQueue *q = queue_lookup(key, uaddr, false);
        uint32_t woken = 0;
        if (q) {
            queue_prune(q);
            while (q->head && woken < val) {
                Thread *t = q->head;
                q->head = t->wq_next;
                t->wq_next = NULL;
                sched_unblock(t, 0);
                woken++;
            }
            queue_release_if_empty(q);
        }
        if (rflags & 0x200) __asm__ volatile("sti");
        return (int64_t)woken;
    }

    case FUTEX_CMP_REQUEUE: {
        volatile uint32_t *addr = (volatile uint32_t *)uaddr;
        uint32_t nr_requeue = (uint32_t)timeout_arg; /* arg4 = val2 */
        uint64_t uaddr2_key_addr = uaddr2;

        uint64_t rflags;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

        if (*addr != val3) {
            if (rflags & 0x200) __asm__ volatile("sti");
            return -11; /* -EAGAIN */
        }

        WaitQueue *q = queue_lookup(key, uaddr, false);
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
            sched_unblock(t, 0);
            woken++;
        }

        uint32_t moved = 0;
        if (nr_requeue > 0) {
            WaitQueue *q2 = queue_lookup(key, uaddr2_key_addr, true);
            if (q2) {
                /* splice up to nr_requeue remaining waiters over */
                Thread **src = &q->head;
                while (*src && moved < nr_requeue) {
                    Thread *t = *src;
                    *src = t->wq_next;
                    queue_push(q2, t);
                    moved++;
                }
                queue_release_if_empty(q);
                queue_release_if_empty(q2);
            }
        }

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
    WaitQueue *q = queue_lookup(key, uaddr, false);
    uint32_t woken = 0;
    if (q) {
        queue_prune(q);
        while (q->head && woken < n) {
            Thread *t = q->head;
            q->head = t->wq_next;
            t->wq_next = NULL;
            sched_unblock(t, 0);
            woken++;
        }
        queue_release_if_empty(q);
    }
    if (rflags & 0x200) __asm__ volatile("sti");
}
