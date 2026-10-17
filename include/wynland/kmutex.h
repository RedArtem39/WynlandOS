/*
 * WynlandOS - Sleeping, recursive kernel mutexes
 * ==============================================
 * For a lock held across sleeps and long waits (the filesystem across a
 * disk command): a thread that finds it taken sleeps on its wait queue --
 * giving the big kernel lock back meanwhile, like any sleep -- instead of
 * spinning. The holder may take it again (depth counts); the last unlock
 * wakes the waiters. Before the scheduler runs (no current thread) it is a
 * no-op: boot is single-threaded.
 */
#pragma once

#include <wynland/types.h>
#include <wynland/spinlock.h>
#include <wynland/waitqueue.h>

struct Thread;

typedef struct KMutex {
    Spinlock lock;           /* owner/depth and the wait queue's sleep */
    struct Thread *owner;
    uint32_t depth;
    WaitQueue wq;
    const char *name;
} KMutex;

#define KMUTEX_INIT(n) { SPINLOCK_INIT(n), 0, 0, { 0 }, (n) }

void kmutex_lock(KMutex *m);
void kmutex_unlock(KMutex *m);
bool kmutex_held(const KMutex *m);   /* by the current thread */
