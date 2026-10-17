/*
 * WynlandOS - Spinlocks
 * =====================
 * The locks beside the big kernel lock (include/wynland/smp.h): each one
 * guards one piece of data -- the run queue, the physical page allocator,
 * the kernel heap -- for code that may run without the big lock, on
 * several cores at once.
 *
 * Taken with interrupts off on this core (spin_lock_irqsave()): an
 * interrupt handler taking the same lock on the same core would wait for
 * itself. Held only briefly, never across a sleep. While spinning, the core
 * answers TLB flush requests (a holder of another lock may be waiting for
 * this core's flush with its own lock held).
 *
 * Checks: taking a lock this core already holds, and spinning for over a
 * second, are reported on the serial log (with the lock's name and who
 * holds it) -- a deadlock shows up as a message instead of a frozen core.
 */
#pragma once

#include <wynland/types.h>

typedef struct Spinlock {
    volatile uint32_t locked;
    volatile int32_t  cpu;       /* the holder's core id, -1 free */
    const char       *name;
    void             *ip;        /* where the holder took it */
} Spinlock;

#define SPINLOCK_INIT(n) { 0, -1, (n), 0 }

void spin_init(Spinlock *l, const char *name);
/* lock with interrupts off; returns the caller's RFLAGS for the unlock */
uint64_t spin_lock_irqsave(Spinlock *l);
void spin_unlock_irqrestore(Spinlock *l, uint64_t flags);
/* interrupts already off (and they stay so) */
void spin_lock(Spinlock *l);
void spin_unlock(Spinlock *l);
bool spin_trylock(Spinlock *l);
/* held by this core (for assertions) */
bool spin_held(const Spinlock *l);
