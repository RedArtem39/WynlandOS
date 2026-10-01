/*
 * WynlandOS - Process Management
 * ============================================================
 * Phase 0: real per-process address spaces. A Process owns a
 * private PML4 (aliasing the kernel/RAM/framebuffer identity map
 * by pointer, see vmm_new_process_pml4()) and its own fd table.
 */
#include <wynland/process.h>
#include <wynland/sched.h>
#include <wynland/vmm.h>
#include <wynland/elf.h>
#include <wynland/heap.h>
#include <wynland/kfile.h>

extern void serial_write_string(const char *str);

static Process *g_process_list = NULL;
static Process *g_kernel_process = NULL;
static uint64_t g_next_pid = 1;

Process *process_create(PageTable *pml4) {
    Process *p = (Process *)kmalloc(sizeof(Process));
    if (!p) return NULL;
    memset(p, 0, sizeof(Process));
    p->pid = g_next_pid++;
    p->pml4 = pml4;
    p->thread_count = 0;
    p->main_thread = NULL;
    p->brk_start = HEAP_BASE;
    p->brk_current = HEAP_BASE;
    /* own group and session until fork()/spawn() says otherwise */
    p->pgid = p->pid;
    p->sid = p->pid;
    p->next = g_process_list;
    g_process_list = p;
    return p;
}

void process_init(void) {
    /* Process 0: the kernel itself, using the boot-time identity-map PML4.
       sched_init() has already created Thread 0; wire it to this process
       right after so sched_current()->proc is always valid from here on. */
    g_kernel_process = process_create(vmm_get_kernel_pml4());
    g_kernel_process->pid = 0;
    g_kernel_process->uid = 0; /* the kernel itself is root */
    g_kernel_process->main_thread = sched_current();
    sched_current()->proc = g_kernel_process;
    serial_write_string("Process: kernel Process 0 initialized.\r\n");
}

Process *process_list_head(void) {
    return g_process_list;
}

void process_mark_exited(Process *p, int wait_status) {
    if (!p || p->exited) return;
    p->wait_status = wait_status;
    p->exited = true;

    /* a vfork parent sleeping until we exec or die */
    if (p->vfork_shared || !p->vfork_released) {
        p->vfork_released = true;
        waitqueue_wake_all(&p->vfork_wq);
    }

    Process *parent = process_find_by_pid(p->ppid);
    if (parent && !parent->exited) {
        waitqueue_wake_all(&parent->child_wq);
        /* SIGCHLD (17): ignored unless the parent installed a handler */
        extern bool signal_raise_thread(uint64_t tid, uint64_t tgid, int sig);
        if (parent->main_thread && parent->pid != 0)
            signal_raise_thread(parent->main_thread->id, parent->pid, 17);
    }
}

Process *process_kernel(void) {
    return g_kernel_process;
}

Process *process_find_by_pid(uint64_t pid) {
    for (Process *p = g_process_list; p; p = p->next) {
        if (p->pid == pid) return p;
    }
    return NULL;
}

/* Removes `p` from g_process_list and frees the struct outright -- unlike
   a real process exit (see process_teardown(), kernel/syscall.c, which
   keeps the struct alive as a zombie so PID lookups/liveness polling stay
   safe), a process that failed to spawn never became visible as a live
   pid to anything, so there's no one who could be holding a reference to
   reap. Used only by process_spawn()'s own failure paths below. */
static void process_unlink_and_free(Process *p) {
    if (g_process_list == p) {
        g_process_list = p->next;
    } else {
        for (Process *cur = g_process_list; cur; cur = cur->next) {
            if (cur->next == p) { cur->next = p->next; break; }
        }
    }
    kfree(p);
}

Process *process_spawn(const char *path, const char **argv, uint32_t uid) {
    PageTable *new_pml4 = vmm_new_process_pml4();
    if (!new_pml4) {
        serial_write_string("process_spawn: failed to allocate PML4\r\n");
        return NULL;
    }

    Process *p = process_create(new_pml4);
    if (!p) {
        serial_write_string("process_spawn: failed to allocate Process\r\n");
        vmm_destroy_process_pml4(new_pml4);
        return NULL;
    }
    /* Set before any thread of this process can run -- same "assign inside
       the safe window, not after the fact" fix as thread_create_ex()'s
       Thread.proc (see kernel/sched.c) for the exact class of race this
       avoids. */
    p->uid = (uid == PROC_UID_INHERIT) ? sched_current()->proc->uid : uid;
    p->ppid = sched_current()->proc->pid;
    if (sched_current()->proc->pid != 0) {
        p->pgid = sched_current()->proc->pgid;
        p->sid  = sched_current()->proc->sid;
    }
    p->vfork_released = true; /* not a vfork child */

    /* fd inheritance -- mirrors real execve() semantics: everything the
       caller has open carries over to the new process at the same fd
       number, except fds explicitly marked FD_CLOEXEC (via fcntl F_SETFD,
       see kernel/syscall.c). This is what lets a future compositor open a
       pipe, leave one end non-CLOEXEC, and have a spawned client inherit
       it without any bespoke fd-passing mechanism.

       Each inherited fd gets its OWN fresh VfsFile struct (deep copy),
       not a shared pointer to the caller's. Sharing the pointer was tried
       first and is a real use-after-free: SYS_close() (kernel/syscall.c)
       unconditionally kfree()s the VfsFile a closed fd points to. If the
       parent later closes its own copy of an fd it also handed to this
       child (the standard "close the child-facing end after spawning"
       pipe idiom -- new usage this session, see Zerp's spawn_client()),
       the child's fd_table entry is left pointing at freed memory. Caught
       live: Zerp's retile() successfully wrote 20 bytes to its own s2c
       write fd, but the spawned client's read on the inherited read fd
       never saw them -- the child's VfsFile had been freed by the
       parent's close() of its OWN (different fd number, same shared
       struct) copy moments earlier. The underlying kernel object a
       VfsFile's current_cluster indexes into (KPipe for pipes, an
       ShmSegment for SHM, real disk data for files) is untouched by this
       -- it's the small per-fd VfsFile wrapper that needs its own copy
       per process, not the thing it points to. */
    Process *caller = sched_current()->proc;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (caller->fd_table[i] && !(caller->fd_flags[i] & FD_CLOEXEC)) {
            VfsFile *copy = (VfsFile *)kmalloc(sizeof(VfsFile));
            /* OOM partway through inheritance: stop copying rather than
               dereference a NULL kmalloc result -- the process still gets
               created with whatever fds copied so far (same best-effort
               tolerance this loop already had for running out of memory
               entirely, just without the crash). */
            if (!copy) break;
            *copy = *caller->fd_table[i];
            kfile_get(copy); /* sockets/memfds count their fds (kfile.h) */
            p->fd_table[i]  = copy;
            p->fd_flags[i]  = caller->fd_flags[i];
            p->fd_oflags[i] = caller->fd_oflags[i];
        }
    }

    uint64_t entry_point = 0;
    uint64_t stack_top = 0;

    /* elf_load() writes through raw virtual pointers after mapping pages
       into new_pml4 -- those raw writes are resolved via whatever's
       currently in CR3, not the pml4 argument, so we need to temporarily
       switch into the new address space to populate it.

       Doing that with a bare "mov cr3" is NOT enough on its own: elf_load()
       calls into vfs_read()/the AHCI driver, which re-enables interrupts
       while it waits on real disk I/O. If a timer IRQ fires in that window,
       sched_schedule() runs and reloads CR3 based on Thread::proc (see
       sched.c) -- and since this calling thread's ->proc still said "the
       caller's original process" (never told about our manual CR3 switch),
       the very first switch away-and-back would silently stomp our
       temporary CR3 with the wrong page table mid-load, corrupting
       whatever gets written next. So make the scheduler's own bookkeeping
       agree with reality: point this thread's ->proc at the new process
       for the duration of the load, so every switch (preempted or not)
       keeps restoring the *correct* CR3 until we're done. */
    Thread *self = sched_current();
    Process *caller_proc = self->proc;
    self->proc = p;
    __asm__ volatile("mov %0, %%cr3" :: "r"((uint64_t)(uintptr_t)new_pml4) : "memory");

    /* argv, like path, must already be kernel-owned memory (kmalloc'd
       copies, or kernel rodata string literals) by the time it reaches
       here -- elf_load() dereferences it while CR3 is switched to the new
       process's still-empty address space, so a raw pointer into the
       *caller's* userspace memory would fault exactly like the path
       argument already did before Phase 1's SYS_spawn fix (see that
       writeup). SYS_spawn's argv handling below mirrors that same fix. */
    bool ok = elf_load(path, &entry_point, &stack_top, new_pml4, argv, NULL, p);

    self->proc = caller_proc;
    __asm__ volatile("mov %0, %%cr3" :: "r"((uint64_t)(uintptr_t)caller_proc->pml4) : "memory");

    if (!ok) {
        serial_write_string("process_spawn: elf_load failed for ");
        serial_write_string(path);
        serial_write_string("\r\n");
        /* elf_load() already unwound whatever IT mapped (kernel/elf.c); this
           cleans up everything ELSE this function itself is responsible for
           -- the inherited fd copies, the VMA list load_elf_segments() may
           have partially built, and new_pml4/p themselves -- so a failed
           spawn leaves nothing behind (previously all of this, plus the
           Process struct and its PML4, leaked on every failed spawn). */
        process_teardown(p);
        process_unlink_and_free(p);
        return NULL;
    }

    {
        int k = 0;
        while (path[k] && k < (int)sizeof(p->exe_path) - 1) { p->exe_path[k] = path[k]; k++; }
        p->exe_path[k] = 0;
    }
    typedef struct { void *entry; void *stack; } ExecArg;
    extern void user_exec_wrapper(void *arg);
    ExecArg *earg = (ExecArg *)kmalloc(sizeof(ExecArg));
    earg->entry = (void *)entry_point;
    earg->stack = (void *)stack_top;

    /* thread_create_ex(), not thread_create()+override -- see its comment
       in kernel/sched.c for the race this closes (a timer IRQ landing
       between thread_create() returning and a separate `t->proc = p`
       assignment could let the scheduler run this thread with the
       CALLER's page tables still loaded). */
    Thread *t = thread_create_ex(user_exec_wrapper, earg, p);
    p->main_thread = t;
    if (t) thread_fx_default(thread_fx_user(t)); /* a fresh program image */
    p->thread_count = 1;

    return p;
}
