/*
 * WynlandOS - Task Scheduler Implementation
 */

#include <wynland/sched.h>
#include <wynland/heap.h>
#include <wynland/irq.h>
#include <wynland/process.h>
#include <wynland/usercopy.h>

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);

uint8_t *current_fx_user = NULL;

void thread_fx_default(uint8_t *area)
{
    memset(area, 0, 512);
    *(uint16_t *)(area + 0)  = 0x037F; /* FCW: all x87 exceptions masked */
    *(uint32_t *)(area + 24) = 0x1F80; /* MXCSR: all SSE exceptions masked */
}
extern void thread_trampoline(void);
extern void serial_write_string(const char *str);
extern void console_print_string(BootInfo *info, const char *str, uint32_t fg, uint32_t bg);
extern void uint_to_str(uint64_t val, char *buf);


static Thread *thread_list = NULL;
static Thread *current_thread = NULL;
static uint64_t next_thread_id = 1;
static Thread *idle_thread = NULL;

static void disable_interrupts(void) {
    __asm__ volatile("cli");
}

/* Runs whenever every real thread is BLOCKED (Phase 22a). The 100Hz timer
   IRQ preempts this thread like any other (it is RUNNING while executing),
   and each preemption's sched_yield() re-enters sched_schedule(), whose
   deadline pass converts expired futex waits back to READY -- so hlt sleeps
   here cost at most one tick of wake latency. */
static void idle_loop(void *arg) {
    (void)arg;
    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}

void sched_init(void) {
    serial_write_string("Sched: Initializing scheduler...\r\n");

    // Create the main thread structure representing the current running kernel main
    current_thread = (Thread *)kmalloc(sizeof(Thread));
    /* Zero everything first: fields nobody sets explicitly (tls_base,
       clear_tid, ...) were heap garbage. tls_base goes straight into
       IA32_FS_BASE on every switch -- a non-canonical value is a #GP on
       real hardware / KVM (TCG never checked, so it went unnoticed). */
    memset(current_thread, 0, sizeof(Thread));
    thread_fx_default(thread_fx_user(current_thread));
    current_fx_user = thread_fx_user(current_thread);
    current_thread->id = 0;
    current_thread->rsp = 0; // Will be set on context switch
    current_thread->stack_orig = NULL; // Main stack is not dynamically allocated by us
    current_thread->state = THREAD_STATE_RUNNING;
    current_thread->proc = NULL; // Wired up by process_init(), called right after sched_init()
    current_thread->next = current_thread; // Circular linked list
    thread_list = current_thread;
    current_thread->wq = NULL;
    current_thread->wq_next = NULL;
    current_thread->wake_deadline = SCHED_NO_DEADLINE;
    current_thread->wake_result = 0;

    /* Phase 22a: the idle thread. Without it, "all threads blocked" used to
       be a permanent cli;hlt halt inside sched_schedule() -- correct when
       nothing could ever unblock, fatal once real blocking exists (the
       waker needs a CPU to run on, and only the timer can provide one by
       preempting whoever is actually executing).

       Built by hand instead of via thread_create(): sched_init() runs
       BEFORE irq_init() (main.c boot order), and thread_create()'s trailing
       `sti` would enable interrupts before any IDT/PIC exists. Same trampoline
       stack layout as thread_create_ex_tls(), minus the interrupt flag flip. */
    {
        disable_interrupts();
        Thread *t = (Thread *)kmalloc(sizeof(Thread));
        memset(t, 0, sizeof(Thread));
        thread_fx_default(thread_fx_area(t));
        thread_fx_default(thread_fx_user(t));
        t->id = next_thread_id++;
        t->state = THREAD_STATE_READY;
        t->tls_base = 0;
        t->proc = NULL;
        t->wq = NULL;
        t->wq_next = NULL;
        t->wake_deadline = SCHED_NO_DEADLINE;
        t->wake_result = 0;

        t->stack_orig = (uint64_t *)kmalloc(THREAD_STACK_SIZE);
        t->stack_orig[0] = KSTACK_GUARD;
        uint64_t *stack_top = t->stack_orig + (THREAD_STACK_SIZE / 8);
        stack_top = (uint64_t *)((uintptr_t)stack_top & ~0xF);
        *(--stack_top) = (uint64_t)thread_trampoline; // Return address
        *(--stack_top) = 0;                          // RBP
        *(--stack_top) = 0;                          // RBX
        *(--stack_top) = (uint64_t)idle_loop;        // R12 -> entry
        *(--stack_top) = 0;                          // R13 -> arg
        *(--stack_top) = 0;                          // R14
        *(--stack_top) = 0;                          // R15
        t->rsp = (uint64_t)stack_top;

        t->next = current_thread->next;
        current_thread->next = t;
        idle_thread = t;
        /* leave interrupts exactly as the caller had them */
    }

    serial_write_string("Sched: Scheduler initialized successfully.\r\n");
}

Thread *thread_create(void (*entry)(void*), void *arg) {
    return thread_create_ex(entry, arg, NULL);
}

/* proc == NULL: new thread shares the caller's process/address space
   (pthread-style, the only thing SYS_clone here ever does). Passing a
   real Process* (process_spawn()'s use) sets Thread::proc to the correct
   value INSIDE the same disabled-interrupts critical section this
   function already uses, before the `sti` below re-enables interrupts.

   This replaces process_spawn()'s old pattern of calling plain
   thread_create() and then overwriting t->proc = p *after* it returned.
   That two-step version had a real race: thread_create()'s own `sti`
   re-enables interrupts while t->proc still held the WRONG (caller's)
   process, and if a timer IRQ landed in that exact window, the
   scheduler could pick the brand-new thread while its proc and the
   previous thread's proc still looked equal (both the caller's
   process) -- sched_schedule()'s CR3-switch check
   (`if (next_thread->proc != prev_thread->proc)`) then saw no process
   change and skipped reloading CR3, so the new thread's first
   instruction executed with the CALLER's page tables still loaded,
   which don't have the new process's freshly-loaded ELF segments mapped
   -- an instant Page Fault (not-present) right at the entry point.
   Confirmed live via GDB: err_code=0x14 (instruction fetch, user mode,
   not present) with CR3 still pointing at the parent's PML4. Hit
   reliably once the parent process (Zerp) did enough pre-spawn work
   (multiple opens/mmaps/pipes) to shift exactly where the periodic
   100Hz timer IRQ landed relative to this window -- simpler spawners
   like shm_test_parent.elf happened not to lose the race, which is why
   this went unnoticed until Zerp's spawns exercised it. */
Thread *thread_create_ex(void (*entry)(void*), void *arg, struct Process *proc) {
    return thread_create_ex_tls(entry, arg, proc, 0);
}

Thread *thread_create_ex_tls(void (*entry)(void*), void *arg, struct Process *proc, uint64_t tls_base) {
    uint64_t saved_rflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(saved_rflags));
    disable_interrupts();

    Thread *t = (Thread *)kmalloc(sizeof(Thread));
    memset(t, 0, sizeof(Thread));
    /* Kernel-side state: a clean image. User state: a copy of the
       creator's user registers (saved when it entered this syscall), like
       fork()/clone() on Linux; a kernel creator has none -- defaults.
       process_spawn()/execve() reset it for a fresh program image. */
    thread_fx_default(thread_fx_area(t));
    if (current_thread && current_thread->proc && current_thread->proc->pid != 0)
        memcpy(thread_fx_user(t), thread_fx_user(current_thread), 512);
    else
        thread_fx_default(thread_fx_user(t));
    t->id = next_thread_id++;
    t->state = THREAD_STATE_READY;
    t->tls_base = tls_base;
    t->clear_tid = NULL;
    t->sig_pending = 0;
    t->sig_mask = 0;
    t->sig_frame = NULL;
    t->proc = proc ? proc : current_thread->proc;

    // Allocate stack
    t->stack_orig = (uint64_t *)kmalloc(THREAD_STACK_SIZE);
    t->stack_orig[0] = KSTACK_GUARD;
    uint64_t *stack_top = t->stack_orig + (THREAD_STACK_SIZE / 8);
    stack_top = (uint64_t *)((uintptr_t)stack_top & ~0xF);

    // Format stack
    *(--stack_top) = (uint64_t)thread_trampoline; // Return address
    *(--stack_top) = 0;                          // RBP
    *(--stack_top) = 0;                          // RBX
    *(--stack_top) = (uint64_t)entry;            // R12
    *(--stack_top) = (uint64_t)arg;              // R13
    *(--stack_top) = 0;                          // R14
    *(--stack_top) = 0;                          // R15

    t->rsp = (uint64_t)stack_top;

    // Insert into circular linked list after current_thread
    t->next = current_thread->next;
    current_thread->next = t;

    /* Back to the CALLER's interrupt state -- never an unconditional sti.
       Syscalls run with IF=0 (SFMASK), and fork()/clone() call this from
       inside one: the old sti let the timer preempt the rest of the
       syscall. A clone()d thread then started before clone() had set its
       TLS (FS base 0, crash on fs:0x28), and the syscall exit path ran
       with interrupts on -- an IRQ between its `pop rsp` and `sysret`
       pushed its frame onto the USER stack, the parent's "stack smashing
       detected" right after fork(). */
    if (saved_rflags & 0x200) __asm__ volatile("sti");

    return t;
}

void thread_exit(void) {
    disable_interrupts();

    /* Phase 22a (pulls a slice of 22e forward): honor CLONE_CHILD_CLEARTID /
       set_tid_address. Must run while this thread's own address space is
       still loaded and its Process is intact: the userspace word write needs
       this CR3, and the futex wake keys on this process's PML4.
       musl's __thread_list_lock protocol DEPENDS on the kernel doing this:
       every pthread is cloned with ctid = &__thread_list_lock precisely so
       that death releases the list lock even if held. Without it, the first
       exiting pthread left the lock stamped with its own tid and the next
       pthread_create blocked forever on __tl_lock(). */
    if (current_thread->clear_tid) {
        extern void futex_wake_user(uint64_t uaddr, uint32_t n);
        /* The address came from user space (set_tid_address / clone
           CHILD_CLEARTID): validate it like any other user write. A bogus
           or since-unmapped pointer used to be a raw kernel store --
           i.e. any process could panic the kernel just by exiting. */
        uint64_t ua = (uint64_t)(uintptr_t)current_thread->clear_tid;
        if ((ua & 3) == 0 && user_prepare_write(ua, sizeof(uint32_t))) {
            *current_thread->clear_tid = 0;
            futex_wake_user(ua, 1);
        }
        current_thread->clear_tid = NULL;
    }

    current_thread->state = THREAD_STATE_TERMINATED;
    /* Phase 18: mark the owning Process dead here -- but ONLY when the
       process's own main thread exits. The original unconditional
       version let ANY helper thread's death kill the whole process
       status: musl's threaded resolver inside curl (and any other
       pthread user) spawns a short-lived worker sharing the Process;
       the moment it finished, Process.exited flipped true while the
       real main thread was still running -- callers polling Process
       (boot regression hook, future wait4) saw a live program as dead.
       The 1:1 model this field documents is main-thread-exit == process
       exit; helper threads are just threads. */
    if (current_thread->proc &&
        current_thread->proc->main_thread == current_thread) {
        /* normal exit: wait status = exit code << 8 */
        process_mark_exited(current_thread->proc, (current_thread->proc->exit_code & 0xFF) << 8);
    }

    sched_schedule();

    // Never reached
    while (1) {
        __asm__ volatile("hlt");
    }
}

void sched_schedule(void) {
    // Free all terminated threads (except the current one we are running on)
    if (thread_list) {
        Thread *prev = thread_list;
        Thread *curr = thread_list->next;
        do {
            if (curr->state == THREAD_STATE_TERMINATED && curr != current_thread) {
                prev->next = curr->next;
                if (curr == thread_list) {
                    thread_list = curr->next;
                }
                Thread *to_free = curr;
                curr = curr->next;

                /* Real address-space teardown, deferred to exactly here:
                   thread_exit() (above) can't do it itself -- it's still
                   running ON this thread/process's own CR3 at that point,
                   and freeing a process's page tables while the CPU could
                   still walk them for an ordinary kernel-code TLB miss is
                   not safe. By the time a terminated thread reaches this
                   loop from a LATER sched_schedule() call, current_thread
                   (and CR3) has long since moved on -- same reasoning that
                   already made it safe to kfree() the Thread struct itself
                   here instead of in thread_exit(). Only tears down once
                   every thread sharing this process (SYS_clone pthreads
                   included, see kernel/syscall.c) has actually terminated,
                   so a surviving pthread sibling is never left running on
                   a freed address space. process_teardown() itself refuses
                   to touch the kernel process (pid 0). */
                if (to_free->proc) {
                    to_free->proc->thread_count--;
                    if (to_free->proc->exited && to_free->proc->thread_count <= 0) {
                        process_teardown(to_free->proc);
                    }
                }

                if (to_free->stack_orig) {
                    kfree(to_free->stack_orig);
                }
                kfree(to_free);
            } else {
                prev = curr;
                curr = curr->next;
            }
        } while (curr != thread_list);
    }

    /* Phase 22a: deadline pass -- a BLOCKED thread whose futex timeout has
       expired becomes READY with an -ETIMEDOUT wake result. Runs on every
       schedule point (timer preemptions, yields, wakes), which is what
       makes timed waits work without any dedicated timer-wheel machinery:
       worst-case latency is one 10ms tick. */
    {
        extern uint64_t timer_get_ticks(void);
        extern uint64_t timer_get_ms(void);
        uint64_t now = timer_get_ticks();
        uint64_t now_ms = timer_get_ms();
        Thread *t = thread_list;
        if (t) {
            do {
                if (t->state == THREAD_STATE_BLOCKED &&
                    ((t->wake_deadline != SCHED_NO_DEADLINE &&
                      now >= t->wake_deadline) ||
                     (t->wake_deadline_ms != SCHED_NO_DEADLINE &&
                      now_ms >= t->wake_deadline_ms))) {
                    t->wake_result = -110; /* -ETIMEDOUT */
                    t->state = THREAD_STATE_READY;
                }
                t = t->next;
            } while (t != thread_list);
        }
    }

    Thread *prev_thread = current_thread;
    Thread *next_thread = current_thread->next;

    // Find the next READY thread
    while (next_thread != current_thread) {
        if (next_thread->state == THREAD_STATE_READY) {
            break;
        }
        next_thread = next_thread->next;
    }

    // If no other ready thread, and current thread is still running/ready, continue
    if (next_thread->state != THREAD_STATE_READY) {
        if (current_thread->state == THREAD_STATE_RUNNING || current_thread->state == THREAD_STATE_READY) {
            current_thread->state = THREAD_STATE_RUNNING;
            return;
        }

        /* Phase 22a: everything real is BLOCKED. Hand the CPU to the idle
           thread instead of halting forever -- its hlt loop sleeps until the
           next tick, and the tick's own preempt-yield re-runs this scan so
           expired deadlines/wakes get picked up. The blocked thread we came
           from keeps its parked state untouched; it resumes inside its own
           sched_block() frame whenever something marks it READY. */
        if (idle_thread && idle_thread != current_thread &&
            idle_thread->state == THREAD_STATE_READY) {
            next_thread = idle_thread;
        } else if (idle_thread && idle_thread == current_thread) {
            /* already the idle context resuming after hlt: just go back to it */
            current_thread->state = THREAD_STATE_RUNNING;
            return;
        } else {
            serial_write_string("Sched: No ready threads! System halted.\r\n");
            while (1) {
                __asm__ volatile("cli; hlt");
            }
        }
    }

    // Switch to next_thread
    if (prev_thread->state == THREAD_STATE_RUNNING) {
        prev_thread->state = THREAD_STATE_READY;
    }
    next_thread->state = THREAD_STATE_RUNNING;
    current_thread = next_thread;

    // Context switch the Thread-Local Storage (TLS) FS Base
    {
        uint32_t msr = 0xC0000100; // IA32_FS_BASE
        uint64_t val = next_thread->tls_base;
        /* Last line of defence: a non-canonical FS base would #GP right
           here, in the scheduler, with interrupts off. */
        if ((uint64_t)((int64_t)(val << 16) >> 16) != val) val = 0;
        uint32_t low = val & 0xFFFFFFFF;
        uint32_t high = val >> 32;
        __asm__ volatile("wrmsr" :: "c"(msr), "a"(low), "d"(high));
    }

    /* the thread being switched away from ran off the bottom of its stack */
    if (prev_thread && prev_thread->stack_orig &&
        prev_thread->stack_orig[0] != KSTACK_GUARD) {
        serial_write_string("SCHED: KERNEL STACK OVERFLOW\r\n");
        prev_thread->stack_orig[0] = KSTACK_GUARD; /* report once */
    }

    // Update TSS interrupt stack pointer for the new thread
    {
        uint64_t next_rsp0;
        if (next_thread->stack_orig) {
            next_rsp0 = ((uint64_t)next_thread->stack_orig + THREAD_STACK_SIZE) & ~0xFULL;
        } else {
            extern uint64_t gdt_original_tss_rsp0;
            next_rsp0 = gdt_original_tss_rsp0;
        }
        extern void gdt_update_tss_rsp0(uint64_t rsp0);
        gdt_update_tss_rsp0(next_rsp0);
    }

    // Switch address space if the new thread belongs to a different
    // process. Pointer comparison avoids needless CR3 reloads/TLB
    // flushes for same-process thread switches (e.g. the common case
    // today: everything still shares Process 0, the kernel process,
    // until process_spawn() creates real isolated processes).
    if (next_thread->proc != prev_thread->proc && next_thread->proc) {
        uint64_t new_cr3 = (uint64_t)(uintptr_t)next_thread->proc->pml4;
        __asm__ volatile("mov %0, %%cr3" :: "r"(new_cr3) : "memory");
    }

    /* Per-thread x87/SSE state at the switch point (kernel code uses SSE
       too). Without this every thread shared one register file: a
       preemption in the middle of an SSE memcpy handed the next thread's
       values to the first. User state is separate (fx_user, saved/restored
       by the entry stubs). No SSE is used between here and the stack
       switch. */
    if (prev_thread != next_thread) {
        __asm__ volatile("fxsave (%0)"  :: "r"(thread_fx_area(prev_thread)) : "memory");
        __asm__ volatile("fxrstor (%0)" :: "r"(thread_fx_area(next_thread)) : "memory");
        current_fx_user = thread_fx_user(next_thread);
    }
    context_switch(&prev_thread->rsp, next_thread->rsp);
}

void sched_yield(void) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

    sched_schedule();

    if (rflags & 0x200) {
        __asm__ volatile("sti");
    }
}

/* While > 0 the timer does not switch threads (interrupts may be on):
   for a thread that must not be interleaved with others but should not
   hold interrupts off for long either -- a disk command it waits for. */
volatile int g_sched_no_preempt;

void sched_preempt_tick(void) {
    if (g_sched_no_preempt) return;
    if (current_thread && current_thread->state == THREAD_STATE_RUNNING) {
        sched_yield();
    }
}

Thread *sched_current(void) {
    return current_thread;
}

/* ---- Phase 22a: real blocking primitives ---- */

static int64_t sched_block_until(void *wq, uint64_t wake_deadline, uint64_t wake_deadline_ms) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

    Thread *t = current_thread;
    t->wq = wq;
    t->wake_deadline = wake_deadline;
    t->wake_deadline_ms = wake_deadline_ms;
    /* wake_result is written by sched_unblock()/the deadline pass BEFORE
       this thread is marked READY, so no initialization race exists: we
       cannot be scheduled until state leaves BLOCKED. */
    t->state = THREAD_STATE_BLOCKED;

    sched_schedule();
    /* Resumed: either a waker or the deadline pass set our result and made
       us READY. Restore the caller's interrupt flag exactly like
       sched_yield() does. */
    if (rflags & 0x200) {
        __asm__ volatile("sti");
    }
    return t->wake_result;
}

int64_t sched_block(void *wq, uint64_t wake_deadline) {
    return sched_block_until(wq, wake_deadline, SCHED_NO_DEADLINE);
}

int64_t sched_block_ms(void *wq, uint64_t wake_deadline_ms) {
    return sched_block_until(wq, SCHED_NO_DEADLINE, wake_deadline_ms);
}

/* Genuine millisecond sleep: parks the thread (the idle thread's hlt gets
   the CPU if nobody else is runnable) until the 1 kHz clock passes the
   deadline. Replaces nanosleep()'s old sched_yield() spin, which kept the
   CPU 100% busy for the whole sleep. */
void sched_sleep_ms(uint64_t ms) {
    extern uint64_t timer_get_ms(void);
    if (ms == 0) {
        sched_yield();
        return;
    }
    sched_block_until(NULL, SCHED_NO_DEADLINE, timer_get_ms() + ms);
}

void sched_unblock(Thread *t, int64_t result) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

    if (t && t->state == THREAD_STATE_BLOCKED) {
        t->wake_result = result;
        t->state = THREAD_STATE_READY;
        t->wq = NULL;
    }

    if (rflags & 0x200) {
        __asm__ volatile("sti");
    }
}

static uint32_t local_strlen(const char *s) {
    uint32_t len = 0;
    while (*s++) len++;
    return len;
}

void sched_print_tasks(BootInfo *info, uint32_t bg_color) {
    console_print_string(info, "WynlandOS Task Scheduler\n", 0x0000FFFF, bg_color);
    console_print_string(info, "ID   | State      | Stack RSP\n", 0x00CCCCCC, bg_color);
    console_print_string(info, "-----|------------|------------------\n", 0x00888888, bg_color);

    // Save interrupt state and disable interrupts
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

    Thread *t = thread_list;
    if (t) {
        do {
            char buf[32];
            // ID
            uint_to_str(t->id, buf);
            console_print_string(info, buf, 0x00FFFFFF, bg_color);
            int pad = 5 - local_strlen(buf);
            while (pad-- > 0) console_print_string(info, " ", 0, bg_color);
            console_print_string(info, "| ", 0x00888888, bg_color);

            // State
            switch (t->state) {
                case THREAD_STATE_READY:
                    console_print_string(info, "READY      ", 0x00FFFF00, bg_color);
                    break;
                case THREAD_STATE_RUNNING:
                    console_print_string(info, "RUNNING    ", 0x0000FF00, bg_color);
                    break;
                case THREAD_STATE_BLOCKED:
                    console_print_string(info, "BLOCKED    ", 0x00FF8800, bg_color);
                    break;
                case THREAD_STATE_TERMINATED:
                    console_print_string(info, "TERMINATED ", 0x00FF0000, bg_color);
                    break;
            }
            console_print_string(info, "| ", 0x00888888, bg_color);

            // RSP
            extern void uint_to_hex(uint64_t val, char *buf);
            uint_to_hex(t->rsp, buf);
            console_print_string(info, buf, 0x00FFFFFF, bg_color);
            console_print_string(info, "\n", 0, bg_color);

            t = t->next;
        } while (t != thread_list);
    }

    if (rflags & 0x200) {
        __asm__ volatile("sti");
    }
}

Thread *sched_get_thread_list(void) {
    return thread_list;
}

bool sched_kill_thread(uint64_t id) {
    if (id == 0) return false;
    
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));
    
    Thread *t = thread_list;
    bool found = false;
    if (t) {
        do {
            if (t->id == id) {
                t->state = THREAD_STATE_TERMINATED;
                found = true;
                break;
            }
            t = t->next;
        } while (t != thread_list);
    }
    
    if (rflags & 0x200) {
        __asm__ volatile("sti");
    }
    return found;
}
