/*
 * WynlandOS - Task Scheduler Header
 */
#pragma once

#include <wynland/types.h>
#include <wynland/boot_info.h>


#define THREAD_STACK_SIZE 16384 // 16 KB stack

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
} Thread;

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
