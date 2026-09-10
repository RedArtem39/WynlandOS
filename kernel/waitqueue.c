/*
 * WynlandOS - Generic wait queue implementation
 */
#include <wynland/waitqueue.h>
#include <wynland/sched.h>

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

/* Mirrors futex.c's self_unlink(): the waiter removes itself from the
   queue after resuming, regardless of whether it got here via a real
   wake or a timeout -- covers both without the scheduler needing to know
   queue internals. */
static void self_unlink(WaitQueue *q, Thread *t) {
    Thread **pp = &q->head;
    while (*pp) {
        if (*pp == t) {
            *pp = t->wq_next;
            t->wq_next = NULL;
            return;
        }
        pp = &(*pp)->wq_next;
    }
}

int64_t waitqueue_wait(WaitQueue *q, uint64_t deadline) {
    Thread *t = sched_current();
    queue_push(q, t);
    int64_t result = sched_block((void *)q, deadline);
    self_unlink(q, t);
    return result;
}

void waitqueue_wake_all(WaitQueue *q) {
    Thread *t = q->head;
    while (t) {
        Thread *next = t->wq_next;
        if (t->state == THREAD_STATE_BLOCKED && t->wq == (void *)q) {
            sched_unblock(t, 0);
        }
        t = next;
    }
}
