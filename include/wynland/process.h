/*
 * WynlandOS - Process Header
 * ============================================================
 * Phase 0 of the real-process-isolation work: each Process owns
 * its own page table (address space) and its own fd namespace.
 * Threads (see sched.h) point at the Process they belong to;
 * for now it's 1:1 thread:process (SYS_clone here is only ever
 * used for in-process pthread-style thread creation), but the
 * struct is shaped so multiple threads could share one Process
 * later without rework.
 */
#pragma once

#include <wynland/waitqueue.h>
#include <wynland/types.h>
#include <wynland/vmm.h>
#include <wynland/vfs.h>
#include <wynland/vma.h>

#define MAX_OPEN_FILES 128
#define FD_CLOEXEC 1

/* Phase 2 (sec hardening): private per-process heap slot. PML4 index 176
   (0x580000000000) -- strictly between the ET_DYN/PIE load offset (index
   160, 0x500000000000, kernel/elf.c) and the user stack (index 192,
   0x600000000000, kernel/elf.c), and nowhere near the shared kernel/RAM/
   framebuffer identity map (index 0, see vmm_new_process_pml4() in
   kernel/vmm.c) that the OLD single-global brk (kernel/syscall.c, started
   at 0x60000000) collided with -- mapping heap pages there modified a
   page-table chain shared BY POINTER across every process, so one
   process's malloc() could appear in (or corrupt) another's address
   space. One full PML4 slot (512 GB of address space) is reserved for
   it, only ever backed by as many physical pages as the process actually
   grows into. */
#define HEAP_BASE 0x580000000000ULL
#define HEAP_MAX  (HEAP_BASE + 0x8000000000ULL) /* +512GB: one PML4 slot's worth */

struct Thread; /* include/wynland/sched.h -- forward-declared to avoid a
                  header dependency here */

/* Phase 22b: one signal disposition. Handlers live PER-PROCESS (threads
   share them); masks/pending live on Thread. */
typedef struct SigAct {
    uint64_t handler;   /* 0 = default action */
    uint64_t restorer;  /* SA_RESTORER trampoline */
    uint64_t flags;
    uint64_t mask;
} SigAct;

typedef struct Process {
    uint64_t   pid;
    uint64_t   ppid;  /* Phase 6: real parent pid. Set by process_spawn()/
                          fork() (kernel/process.c, kernel/syscall.c) to the
                          caller's pid at creation time; 0 for the kernel
                          process itself (process_init(), the root of the
                          tree -- matches real Linux's pid-1-has-parent-0
                          convention). Never updated afterward (this OS has
                          no reparent-to-init-on-parent-exit behavior). */
    PageTable *pml4;
    VfsFile   *fd_table[MAX_OPEN_FILES];
    uint32_t   fd_flags[MAX_OPEN_FILES];   /* Per-fd flags (FD_CLOEXEC etc.) */
    uint32_t   fd_oflags[MAX_OPEN_FILES];  /* Per-fd open status flags */
    int        thread_count;
    uint32_t   uid;              /* Phase 5: real per-process UID. 0 = root. */
    struct Thread  *main_thread; /* entry thread -- lets callers poll for exit */
    bool       exited;           /* Phase 18: set by thread_exit() itself (see
                                     sched.c) rather than read via main_thread,
                                     since a terminated Thread struct can be
                                     kfree()'d by a later, unrelated
                                     sched_schedule() call -- Process itself
                                     is never freed, so this flag is safe to
                                     poll from a syscall at any time. */
    /* Phase 22b: signal dispositions are PER-PROCESS (threads share them);
       masks/pending live on Thread. Zeroed by process_create()'s memset. */
    SigAct sig_acts[65];
    /* Phase 22c: real VMAs -- every mapped range (ELF segments, stack, mmap
       regions) is registered here so mprotect/munmap/the page fault
       handler's COW path have something to look up. Zeroed by
       process_create()'s memset (NULL = empty list). */
    struct VMA *vma_list;
    /* Phase 2 (sec hardening): process-local brk state, private to this
       address space -- see HEAP_BASE/HEAP_MAX above. brk_start is the
       fixed base (== HEAP_BASE for every process); brk_current is the
       current break, grown/shrunk by SYS_brk (kernel/syscall.c). Set by
       process_create() and copied by fork(); execve() resets brk_current
       back to brk_start for the new image. */
    uint64_t brk_start;
    uint64_t brk_current;
    /* next free address of each mmap() slot (MMAP_SLOT_*, kernel/syscall.c);
       0 = slot unused yet. Per process: they were global, so every mmap of
       every process ever pushed them up until one slot ran into the next. */
    uint64_t mmap_next[6];
    /* JIT: anonymous memory may be writable and executable at once
       (JavaScriptCore's JIT on Linux wants RWX). Off by default -- W^X --
       on for a program started with WYNLAND_ALLOW_JIT=1 in its
       environment, inherited by fork(), decided again at every execve(). */
    bool allow_wx;
    /* ---- process lifecycle: exit status, wait4/waitid, vfork ---- */
    int        exit_code;    /* exit()/exit_group() argument, kept until death */
    int        wait_status;  /* Linux wait status once exited: code<<8, or the
                                terminating signal number */
    bool       reaped;       /* already collected by wait4()/waitid() */
    uint64_t   pgid;         /* process group */
    uint64_t   sid;          /* session */
    WaitQueue  child_wq;     /* this process sleeps here in wait4()/waitid();
                                a child's death wakes it */
    /* vfork (and clone(CLONE_VM|CLONE_VFORK), what glibc's posix_spawn uses):
       the child runs in the PARENT's address space (pml4 is shared, not
       copied) until it execve()s or exits; the parent sleeps on vfork_wq
       until vfork_released. While vfork_shared, nothing may free or replace
       this pml4 on the child's behalf. */
    bool       vfork_shared;
    volatile bool vfork_released;
    WaitQueue  vfork_wq;
    /* path of the running image (process_spawn/execve, inherited by fork):
       what readlink("/proc/self/exe") returns */
    char       exe_path[128];
    struct Process *next;
} Process;

/* Sentinel for process_spawn()'s uid parameter: inherit the calling
   process's current uid (normal fork+exec-preserves-privilege rule),
   rather than forcing a specific one. */
#define PROC_UID_INHERIT ((uint32_t)-1)

void      process_init(void);          /* Creates Process 0 (kernel), uid = 0 */
/* The process died with Linux wait status `wait_status` (code<<8 for a
   normal exit, the signal number for a fatal signal). Idempotent: the
   first call wins. Marks it exited, releases a vfork parent, wakes the
   parent's wait4()/waitid() and sends it SIGCHLD. */
void      process_mark_exited(Process *p, int wait_status);
Process  *process_list_head(void);      /* walk with ->next */
Process  *process_create(PageTable *pml4);
Process  *process_kernel(void);         /* Process 0 -- pml4 = boot snapshot */
Process  *process_find_by_pid(uint64_t pid); /* Phase 18: NULL if never existed. Never freed once created (see g_process_list), so safe to hold across calls -- check ->exited, don't assume liveness from non-NULL alone. */
Process  *process_spawn(const char *path, const char **argv, uint32_t uid); /* Loads path into a fresh address space, spawns its entry thread. argv may be NULL (historical default args) or a NULL-terminated array of kernel-owned strings. uid: PROC_UID_INHERIT to keep the caller's current uid, or a specific value (e.g. boot launches demoting to 1000). */

/* Releases everything real about a process: every open fd (type-specific
   cleanup -- closes real TCP connections, real UDP sockets, drops SHM
   refcounts -- same as a live close() would do), the VMA list, and the
   entire private address space (vmm_destroy_process_pml4(), which also
   frees the PML4 root itself -- proc->pml4 is NULL after this call).
   Defined in kernel/syscall.c (needs that file's pipe/PTY/socket/SHM
   tables). Does NOT free or unlink the Process struct itself or touch
   ->exited/->pid -- callers decide that part: a real process exit
   (kernel/sched.c) keeps the struct alive as a zombie so PID lookups via
   process_find_by_pid() stay safe; process_spawn()'s own failure path
   (this file) unlinks and frees it outright since a process that never
   finished spawning was never visible as a live pid to anything. Safe to
   call on a process that already has no address space (idempotent). */
void process_teardown(Process *proc);
