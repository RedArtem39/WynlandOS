/*
 * WynlandOS - Task Scheduler Header
 */
#pragma once

#include <wynland/types.h>
#include <wynland/boot_info.h>


/* Kernel stack per thread. syscall_dispatcher's frame alone is ~7 KB and
   dup2()/sendto() re-enter it, plus the network send path (~2 KB frames)
   and nested IRQ frames: 16 KB overflowed into the neighbouring heap block
   -- often another thread's kernel stack with its saved user registers,
   which came back as glibc "stack smashing detected" in that process. */
#define THREAD_STACK_SIZE 65536 // 64 KB stack
/* Written at the lowest word of every kernel stack, checked at each
   context switch: an overflow is reported instead of silently corrupting
   the heap. */
#define KSTACK_GUARD 0x57AC6A4D57AC6A4DULL

/* Passed as wake_deadline to sched_block(): never time out on its own. */
#define SCHED_NO_DEADLINE 0ULL

typedef enum {
    THREAD_STATE_READY,
    THREAD_STATE_RUNNING,
    THREAD_STATE_BLOCKED,
    THREAD_STATE_TERMINATED
} ThreadState;

struct Process; /* include/wynland/process.h -- forward-declared to avoid a
                   header dependency here; only .c files that dereference
                   Thread::proc need to #include <wynland/process.h> */

typedef struct Thread {
    uint64_t id;
    uint64_t rsp;             // Saved stack pointer (RSP)
    uint64_t *stack_orig;     // Pointer returned by kmalloc (for freeing)
    ThreadState state;
    uint64_t tls_base;        // Thread-Local Storage base address (FS segment base)
    struct Process *proc;     // Process this thread belongs to (address space, fd table)
    struct Thread *next;
    /* ---- Phase 22a: real blocking (sleep/wake) support ---- */
    void *wq;                 /* opaque wait queue this thread is parked on
                                 (futex.c-owned; NULL while runnable). Because
                                 every thread owns a persistent kernel stack,
                                 blocking = park + schedule away; resuming
                                 continues INSIDE sched_block()'s frame and its
                                 return value carries the wake result. */
    struct Thread *wq_next;   /* intrusive FIFO link within that queue */
    uint64_t wake_deadline;   /* absolute timer ticks after which the scheduler
                                 unblocks with -ETIMEDOUT; SCHED_NO_DEADLINE */
    uint64_t wake_deadline_ms; /* same, on the 1 kHz timer_get_ms() clock
                                  (sched_sleep_ms()); SCHED_NO_DEADLINE.
                                  Only read while BLOCKED, and every block
                                  path sets it, so it needs no init. */
    int64_t wake_result;      /* value sched_block() returns once resumed */
    uint32_t *clear_tid;      /* CLONE_CHILD_CLEARTID / set_tid_address target:
                                 on this thread's death the kernel writes 0 here
                                 and futex-wakes it. musl's thread-list lock
                                 DEPENDS on this (it passes &__thread_list_lock
                                 as ctid for every pthread and relies on the
                                 kernel to release it if the holder dies). */
    /* ---- Phase 22b: signal state (per-thread; dispositions per-Process) */
    uint64_t sig_pending;     /* bitmask, bit N = signal N pending */
    uint64_t sig_mask;        /* blocked-set (9 KILL / 19 STOP unblockable) */
    void *sig_frame;          /* kernel-side frame copy for SYS_rt_sigreturn */
    /* Two x87/SSE register images (FXSAVE, 512 bytes, 16-byte aligned via
       the accessors below). Kernel C code uses SSE too (it is NOT built
       with -mno-sse: GUI/OpenGL/QuickJS need floating point), so:
       - fx_user: the thread's USER state, saved on every entry into the
         kernel from user mode (syscall, interrupt, exception) and restored
         on the way back -- what Linux guarantees to user code;
       - fx_raw: the live state at a context switch, i.e. whatever kernel
         code was doing with the registers when it got preempted.
       Last fields on purpose: nothing before them moves. */
    uint8_t fx_raw[512 + 16];
    uint8_t fx_user_raw[512 + 16];
} Thread;

static inline uint8_t *thread_fx_area(Thread *t)
{
    return (uint8_t *)(((uintptr_t)t->fx_raw + 15) & ~(uintptr_t)15);
}

static inline uint8_t *thread_fx_user(Thread *t)
{
    return (uint8_t *)(((uintptr_t)t->fx_user_raw + 15) & ~(uintptr_t)15);
}

/* The running thread's fx_user image: the syscall/interrupt entry stubs
   FXSAVE into it on entry from user mode and FXRSTOR from it on return. */
extern uint8_t *current_fx_user;

/* Power-on-like user FPU state: x87 control word 0x37F, MXCSR 0x1F80
   (all exceptions masked), everything else zero. */
void thread_fx_default(uint8_t *area);

void sched_init(void);
Thread *thread_create(void (*entry)(void*), void *arg);
Thread *thread_create_ex(void (*entry)(void*), void *arg, struct Process *proc); /* proc == NULL means "inherit caller's proc", same as thread_create() */
/* Same as thread_create_ex(), but also sets Thread.tls_base inside the same
   disabled-interrupts critical section (closing the same class of race
   Thread.proc's in-window assignment closes) instead of leaving it at the
   default 0 thread_create_ex() uses. Needed by real fork() (kernel/syscall.c)
   so the child resumes with the SAME FS_BASE the parent had at the moment
   of the fork() syscall, not a fresh/zero one. */
Thread *thread_create_ex_tls(void (*entry)(void*), void *arg, struct Process *proc, uint64_t tls_base);
void thread_exit(void);
void sched_yield(void);
void sched_schedule(void);
Thread *sched_current(void);
void sched_preempt_tick(void);
void sched_print_tasks(BootInfo *info, uint32_t bg_color);
Thread *sched_get_thread_list(void);
bool sched_kill_thread(uint64_t id);

/* Phase 22a blocking primitives. sched_block() parks the CURRENT thread on
   the opaque wait queue `wq` until sched_wake_* targets it (returns
   *result) or wake_deadline ticks elapse (returns -ETIMEDOUT). Callers must
   hold interrupts disabled OR accept the internal cli window; the primitive
   saves/restores the caller's IF flag exactly like sched_yield().
   sched_unblock() marks one parked thread READY with its wake result; safe
   from any context with interrupts disabled internally. */
int64_t sched_block(void *wq, uint64_t wake_deadline);
void sched_unblock(Thread *t, int64_t result);
/* sched_block() with the deadline on the 1 kHz timer_get_ms() clock. */
int64_t sched_block_ms(void *wq, uint64_t wake_deadline_ms);
/* Park the current thread for `ms` milliseconds of the 1 kHz clock
   (0 = plain yield). Not a spin: the CPU is free for the whole sleep. */
void sched_sleep_ms(uint64_t ms);
