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

#include <wynland/types.h>
#include <wynland/vmm.h>
#include <wynland/vfs.h>
#include <wynland/vma.h>

#define MAX_OPEN_FILES 128
#define FD_CLOEXEC 1

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
    struct Process *next;
} Process;

/* Sentinel for process_spawn()'s uid parameter: inherit the calling
   process's current uid (normal fork+exec-preserves-privilege rule),
   rather than forcing a specific one. */
#define PROC_UID_INHERIT ((uint32_t)-1)

void      process_init(void);          /* Creates Process 0 (kernel), uid = 0 */
Process  *process_create(PageTable *pml4);
Process  *process_kernel(void);         /* Process 0 -- pml4 = boot snapshot */
Process  *process_find_by_pid(uint64_t pid); /* Phase 18: NULL if never existed. Never freed once created (see g_process_list), so safe to hold across calls -- check ->exited, don't assume liveness from non-NULL alone. */
Process  *process_spawn(const char *path, const char **argv, uint32_t uid); /* Loads path into a fresh address space, spawns its entry thread. argv may be NULL (historical default args) or a NULL-terminated array of kernel-owned strings. uid: PROC_UID_INHERIT to keep the caller's current uid, or a specific value (e.g. boot launches demoting to 1000). */
