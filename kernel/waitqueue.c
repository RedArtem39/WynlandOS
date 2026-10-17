/*
 * WynlandOS - Generic wait queue implementation
 *
 * Every queue's list is under g_wq_lock (one lock for all of them: the
 * lists are short and touched briefly). A waiter is on its queue and
 * BLOCKED before the lock goes (sched_block_unlock_ex()), so a waker that
 * takes the lock after the waiter's last look at its condition finds it.
 * Lock order: an object's own lock (a pipe's) -> g_wq_lock -> the
 * scheduler's.
 */
#include <wynland/waitqueue.h>
#include <wynland/sched.h>
#include <wynland/spinlock.h>

static Spinlock g_wq_lock = SPINLOCK_INIT("waitqueue");
static volatile uint64_t g_wake_seq;

uint64_t waitqueue_lock_irqsave(void) { return spin_lock_irqsave(&g_wq_lock); }
void waitqueue_unlock_irqrestore(uint64_t f) { spin_unlock_irqrestore(&g_wq_lock, f); }

uint64_t waitqueue_seq(void)
{
    return __atomic_load_n(&g_wake_seq, __ATOMIC_SEQ_CST);
}

static void queue_push(WaitQueue *q, Thread *t) {
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

/* Mirrors futex.c's self_unlink(): the waiter removes itself from the
   queue after resuming, regardless of whether it got here via a real
   wake or a timeout -- covers both without the scheduler needing to know
   queue internals. g_wq_lock held. */
static void self_unlink(WaitQueue *q, Thread *t) {
    Thread **pp = &q->head;
    while (*pp) {
        if (*pp == t) {
            *pp = t->wq_next;
            t->wq_next = NULL;
            t->wq_head = NULL;
            return;
        }
        pp = &(*pp)->wq_next;
    }
}

/* held: a lock of the caller's to let go once parked (NULL: none). seq:
   don't sleep at all if a wake-up happened since the caller read
   waitqueue_seq() (has_seq). */
static int64_t wait_common(WaitQueue *q, uint64_t deadline, uint64_t deadline_ms,
                           Spinlock *held, bool has_seq, uint64_t seq)
{
    Thread *t = sched_current();
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    spin_lock(&g_wq_lock);
    if (held) spin_unlock(held);
    if (has_seq && __atomic_load_n(&g_wake_seq, __ATOMIC_SEQ_CST) != seq) {
        spin_unlock(&g_wq_lock);
        if (f & 0x200) __asm__ volatile("sti" ::: "memory");
        return 0;   /* something changed meanwhile: the caller looks again */
    }
    queue_push(q, t);
    int64_t result = sched_block_unlock_ex((void *)q, deadline, deadline_ms, &g_wq_lock, false);
    spin_lock(&g_wq_lock);
    self_unlink(q, t);
    spin_unlock(&g_wq_lock);
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
    return result;
}

int64_t waitqueue_wait(WaitQueue *q, uint64_t deadline) {
    return wait_common(q, deadline, SCHED_NO_DEADLINE, NULL, false, 0);
}

int64_t waitqueue_wait_ms(WaitQueue *q, uint64_t deadline_ms) {
    return wait_common(q, SCHED_NO_DEADLINE, deadline_ms, NULL, false, 0);
}

int64_t waitqueue_wait_ms_seq(WaitQueue *q, uint64_t deadline_ms, uint64_t seq) {
    return wait_common(q, SCHED_NO_DEADLINE, deadline_ms, NULL, true, seq);
}

int64_t waitqueue_wait_locked(WaitQueue *q, uint64_t deadline_ms, Spinlock *held) {
    return wait_common(q, SCHED_NO_DEADLINE, deadline_ms, held, false, 0);
}

void waitqueue_wake_all(WaitQueue *q) {
    uint64_t f = spin_lock_irqsave(&g_wq_lock);
    __atomic_add_fetch(&g_wake_seq, 1, __ATOMIC_SEQ_CST);
    Thread *t = q->head;
    while (t) {
        Thread *next = t->wq_next;
        if (t->state == THREAD_STATE_BLOCKED && t->wq == (void *)q) {
            sched_unblock(t, 0);
        }
        t = next;
    }
    spin_unlock_irqrestore(&g_wq_lock, f);
}
