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

typedef struct Thread {
    uint64_t id;
    uint64_t rsp;             // Saved stack pointer (RSP)
    uint64_t *stack_orig;     // Pointer returned by kmalloc (for freeing)
    ThreadState state;
    struct Thread *next;
} Thread;

void sched_init(void);
Thread *thread_create(void (*entry)(void*), void *arg);
void thread_exit(void);
void sched_yield(void);
void sched_schedule(void);
Thread *sched_current(void);
void sched_preempt_tick(void);
void sched_print_tasks(BootInfo *info, uint32_t bg_color);
