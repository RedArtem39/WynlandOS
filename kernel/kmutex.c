/*
 * WynlandOS - Sleeping, recursive kernel mutexes (include/wynland/kmutex.h)
 */
#include <wynland/kmutex.h>
#include <wynland/sched.h>

void kmutex_lock(KMutex *m)
{
    Thread *me = sched_current();
    if (!me) return;
    uint64_t f = spin_lock_irqsave(&m->lock);
    if (m->owner == me) {
        m->depth++;
        spin_unlock_irqrestore(&m->lock, f);
        return;
    }
    while (m->owner) {
        /* parked before the lock goes (no lost wake-up); a dying thread
           is not parked at all -- it still needs the mutex to finish
           (its files close through it), so it waits by yielding */
        if (sched_dying()) {
            spin_unlock_irqrestore(&m->lock, f);
            sched_yield();
            f = spin_lock_irqsave(&m->lock);
            continue;
        }
        waitqueue_wait_locked(&m->wq, SCHED_NO_DEADLINE, &m->lock);
        spin_lock(&m->lock);
    }
    m->owner = me;
    m->depth = 1;
    spin_unlock_irqrestore(&m->lock, f);
}

void kmutex_unlock(KMutex *m)
{
    Thread *me = sched_current();
    if (!me) return;
    uint64_t f = spin_lock_irqsave(&m->lock);
    if (m->owner != me) {
        spin_unlock_irqrestore(&m->lock, f);
        extern void serial_write_string(const char *);
        serial_write_string("KMUTEX: unlocked by a thread that does not hold it: ");
        serial_write_string(m->name ? m->name : "?");
        serial_write_string("\r\n");
        return;
    }
    bool last = --m->depth == 0;
    if (last) m->owner = NULL;
    spin_unlock_irqrestore(&m->lock, f);
    if (last) waitqueue_wake_all(&m->wq);
}

bool kmutex_trylock(KMutex *m)
{
    Thread *me = sched_current();
    if (!me) return true;
    uint64_t f = spin_lock_irqsave(&m->lock);
    bool ok = true;
    if (m->owner == me) m->depth++;
    else if (!m->owner) { m->owner = me; m->depth = 1; }
    else ok = false;
    spin_unlock_irqrestore(&m->lock, f);
    return ok;
}

bool kmutex_held(const KMutex *m)
{
    Thread *me = sched_current();
    return me && m->owner == me;
}
