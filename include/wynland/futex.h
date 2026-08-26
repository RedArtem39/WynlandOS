/*
 * WynlandOS - real futex support (Phase 22a)
 *
 * Replaces SYS_futex's documented EAGAIN+yield approximation with actual
 * sleep/wake queues. This is the foundation everything else in Phase 22
 * stands on: pthread_join, pthread condvars/mutexes under contention,
 * epoll's sleeper model -- all reduce to "block me on this address until
 * someone wakes it or I time out".
 *
 * Keying: (process PML4 physical address, uaddr). Threads of one process
 * share the key naturally (that covers musl's private futexes AND its
 * non-private same-process usage). Cross-process shared-memory futexes
 * would need physical-page keying -- deliberately deferred until something
 * needs them (documented limitation, not an oversight).
 */
#pragma once

#include <wynland/types.h>

/* Full SYS_futex entry point. `val3` is the 6th Linux-register argument
   (r9) -- only FUTEX_CMP_REQUEUE reads it. Returns: >=0 wake/moved counts
   for WAKE ops, 0 for successful waits, or negative errno (-EAGAIN,
   -ETIMEDOUT, -ENOMEM, -ENOSYS, -EFAULT). */
int64_t futex_syscall(uint64_t uaddr, uint32_t op_raw, uint32_t val,
                      uint64_t timeout_arg, uint64_t uaddr2, uint32_t val3);

void futex_init(void);
