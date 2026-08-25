/*
 * WynlandOS - Task Scheduler Implementation
 */

#include <wynland/sched.h>
#include <wynland/heap.h>
#include <wynland/irq.h>
#include <wynland/process.h>

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern void thread_trampoline(void);
extern void serial_write_string(const char *str);
extern void console_print_string(BootInfo *info, const char *str, uint32_t fg, uint32_t bg);
extern void uint_to_str(uint64_t val, char *buf);


static Thread *thread_list = NULL;
static Thread *current_thread = NULL;
static uint64_t next_thread_id = 1;


static void disable_interrupts(void) {
    __asm__ volatile("cli");
}

void sched_init(void) {
    serial_write_string("Sched: Initializing scheduler...\r\n");

    // Create the main thread structure representing the current running kernel main
    current_thread = (Thread *)kmalloc(sizeof(Thread));
    current_thread->id = 0;
    current_thread->rsp = 0; // Will be set on context switch
    current_thread->stack_orig = NULL; // Main stack is not dynamically allocated by us
    current_thread->state = THREAD_STATE_RUNNING;
    current_thread->proc = NULL; // Wired up by process_init(), called right after sched_init()
    current_thread->next = current_thread; // Circular linked list
    thread_list = current_thread;

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
    disable_interrupts();

    Thread *t = (Thread *)kmalloc(sizeof(Thread));
    t->id = next_thread_id++;
    t->state = THREAD_STATE_READY;
    t->tls_base = tls_base;
    t->proc = proc ? proc : current_thread->proc;

    // Allocate stack
    t->stack_orig = (uint64_t *)kmalloc(THREAD_STACK_SIZE);
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

    // Enable interrupts
    __asm__ volatile("sti");

    return t;
}

void thread_exit(void) {
    disable_interrupts();

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
        current_thread->proc->exited = true;
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
        
        // Deadlock/idle condition: no ready threads left
        serial_write_string("Sched: No ready threads! System halted.\r\n");
        while (1) {
            __asm__ volatile("cli; hlt");
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
        uint32_t low = val & 0xFFFFFFFF;
        uint32_t high = val >> 32;
        __asm__ volatile("wrmsr" :: "c"(msr), "a"(low), "d"(high));
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

void sched_preempt_tick(void) {
    if (current_thread && current_thread->state == THREAD_STATE_RUNNING) {
        sched_yield();
    }
}

Thread *sched_current(void) {
    return current_thread;
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
