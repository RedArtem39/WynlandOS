/*
 * WynlandOS - Generic wait queue
 * ============================================================
 * Phase 22d: real blocking I/O. Same push/self-unlink pattern as
 * kernel/futex.c's per-(pml4,uaddr) WaitQueue, but keyed by pointer
 * identity instead of a userspace address -- meant to be embedded
 * directly in a KPipe/Pty/TCP-connection/UDP-socket struct rather than
 * looked up in a global table, since those objects already have a
 * stable kernel-side identity of their own.
 */
#pragma once

#include <wynland/types.h>

struct Thread; /* include/wynland/sched.h -- forward-declared */

typedef struct WaitQueue {
    struct Thread *head; /* intrusive FIFO via Thread.wq_next */
} WaitQueue;

/* Blocks the calling thread on `q` until woken by waitqueue_wake_all(q) or
   until `deadline` (absolute timer ticks, SCHED_NO_DEADLINE to wait
   forever). Returns the wake result: 0 on a real wake, -ETIMEDOUT (-110)
   on timeout. Callers must re-check their own condition after this
   returns -- multiple waiters can be woken for one event, and a timeout
   race can also return 0 spuriously close to the deadline (same contract
   sched_block() already documents). */
int64_t waitqueue_wait(WaitQueue *q, uint64_t deadline);

/* Wakes every thread currently parked on `q`. Safe to call from a normal
   thread (producer just wrote/read something) or from interrupt context
   (net_poll() driven off the timer tick, Phase 22d) -- it only flips
   already-BLOCKED threads to READY, the woken threads unlink themselves
   from `q` the next time they run (see waitqueue_wait()). */
void waitqueue_wake_all(WaitQueue *q);
