/*
 * WynlandOS - Task Scheduler Implementation
 */

#include <wynland/sched.h>
#include <wynland/heap.h>
#include <wynland/irq.h>
#include <wynland/process.h>
#include <wynland/usercopy.h>
#include <wynland/vma.h>
#include <wynland/vmm.h>
#include <wynland/smp.h>
#include <wynland/spinlock.h>
#include <wynland/futex.h>
#include <wynland/waitqueue.h>

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);

uint8_t g_fpu_xsave = 0;

extern void serial_write_string(const char *str);

void fpu_init(void)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c & (1u << 26))) {                       /* no XSAVE: FXSAVE, no AVX */
        serial_write_string("FPU: FXSAVE (x87, SSE)\r\n");
        return;
    }
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 1ULL << 18;                             /* OSXSAVE */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
    uint64_t xcr0 = 0x3;                           /* x87 | SSE */
    if (c & (1u << 28)) xcr0 |= 0x4;               /* AVX (YMM upper halves) */
    __asm__ volatile("xsetbv" :: "c"(0), "a"((uint32_t)xcr0), "d"((uint32_t)(xcr0 >> 32)));
    /* the save area those components need must fit FPU_AREA_MAX */
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0xD), "c"(0));
    if (b > FPU_AREA_MAX) {
        xcr0 = 0x3;
        __asm__ volatile("xsetbv" :: "c"(0), "a"((uint32_t)xcr0), "d"(0u));
    }
    g_fpu_xsave = 1;
    serial_write_string((xcr0 & 0x4) ? "FPU: XSAVE (x87, SSE, AVX)\r\n" : "FPU: XSAVE (x87, SSE)\r\n");
}

void thread_fx_default(uint8_t *area)
{
    /* zero XSAVE header too: XRSTOR then gives each component its
       initial state (x87 control word 0x37F); MXCSR is read from here */
    memset(area, 0, FPU_AREA_MAX);
    *(uint16_t *)(area + 0)  = 0x037F; /* FCW: all x87 exceptions masked */
    *(uint32_t *)(area + 24) = 0x1F80; /* MXCSR: all SSE exceptions masked */
}
extern void thread_trampoline(void);
extern void serial_write_string(const char *str);
extern void console_print_string(BootInfo *info, const char *str, uint32_t fg, uint32_t bg);
extern void uint_to_str(uint64_t val, char *buf);


static Thread *thread_list = NULL;
static uint64_t next_thread_id = 1;

/* The thread running on this core (include/wynland/smp.h: each core
   runs its own; GS finds the core's data) */
#define current_thread (this_cpu()->current)

static void disable_interrupts(void) {
    __asm__ volatile("cli");
}

/* ==== EEVDF: Earliest Eligible Virtual Deadline First ===================
   (Stoica & Abdel-Wahab 1995; Linux's fair scheduler since 6.6.)

   Every thread accrues virtual run time: its time on a core scaled by
   1024/weight (the weight follows nice: 1024 at 0, x1.25 per step down),
   so a heavier thread's clock runs slower and it gets more of the cores.
   V is the weighted average of the vruntimes of all runnable threads (the
   queued ones and those on a core). A thread with vruntime <= V has been
   served no more than its share: it is *eligible*. Each thread asks for a
   slice; its virtual deadline is vruntime + slice x 1024/weight. The pick
   is the eligible thread with the earliest deadline: short slices come
   round soon, nobody runs ahead of its share for long.

   A thread that sleeps keeps its lag (V - vruntime) and comes back at
   V - lag: sleeping neither earns credit nor costs it. A new thread
   starts at V with half a slice (it runs soon, without jumping ahead).

   Data: READY threads waiting for a core are on one run queue (all cores
   pick from it); BLOCKED threads with a timeout on a timer list sorted by
   wake-up time. Neither is the full thread list: a pick costs the number
   of runnable threads, not of all threads. All of it under g_sched_lock
   (interrupts off), not the big kernel lock: a core switches threads, and
   wakes them, while another one is in the kernel.

   The lock is held across a switch: the thread leaving the core is back
   on the run queue before its registers are saved, so no other core may
   pick it until the switch is done -- the thread switched TO lets go
   (sched_finish(): after context_switch(), or in thread_trampoline for a
   new one), then takes the big lock again if it was inside it. */

#define NICE_0_WEIGHT   1024
#define SCHED_SLICE_NS  3000000ULL      /* each request: 3 ms of a core */

/* Linux's table: weight(nice) = 1024 / 1.25^nice */
static const uint32_t g_nice_weight[40] = {
    88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
     9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
     1024,   820,   655,   526,   423,   335,   272,   215,   172,   137,
      110,    87,    70,    56,    45,    36,    29,    23,    18,    15,
};

static Thread *g_rq;                 /* the run queue (doubly linked) */
static uint32_t g_rq_count;
static Thread *g_timers;             /* BLOCKED with a deadline, soonest first */
static uint64_t g_v_last;            /* V when last computed: for wake-ups into an empty system */
static volatile uint32_t g_dead;     /* TERMINATED threads not yet freed */
static Spinlock g_sched_lock = SPINLOCK_INIT("sched");
static void reaper_kick(void);

/* ---- the clock ---- */
static uint64_t g_tsc0, g_ms0, g_tsc_per_ms;

static inline uint64_t rdtsc_now(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* The TSC's rate against the 1 kHz timer: 50 ms of it, from a tick edge,
   with interrupts on (kernel/smp.c, at boot) -- the millisecond count
   falls behind while interrupts are off, so it is no clock for this
   anywhere else. Until then the clock is the millisecond count. */
void sched_clock_calibrate(void)
{
    extern uint64_t timer_get_ms(void);
    uint64_t m0 = timer_get_ms();
    while (timer_get_ms() == m0) __builtin_ia32_pause();
    uint64_t t0 = rdtsc_now(), ms0 = timer_get_ms();
    while (timer_get_ms() - ms0 < 50) __builtin_ia32_pause();
    uint64_t per = (rdtsc_now() - t0) / (timer_get_ms() - ms0);
    if (!per) return;
    g_tsc0 = t0;
    g_ms0 = ms0;
    g_tsc_per_ms = per;
}

uint64_t sched_clock_ns(void)
{
    extern uint64_t timer_get_ms(void);
    if (!g_tsc_per_ms) return timer_get_ms() * 1000000ULL;
    uint64_t d = rdtsc_now() - g_tsc0;
    return g_ms0 * 1000000ULL + d / g_tsc_per_ms * 1000000ULL +
           d % g_tsc_per_ms * 1000000ULL / g_tsc_per_ms;
}

static inline uint64_t vslice_of(const Thread *t)
{
    return SCHED_SLICE_NS * NICE_0_WEIGHT / t->weight;
}

/* a before b in virtual time (wrap-safe) */
static inline bool vbefore(uint64_t a, uint64_t b)
{
    return (int64_t)(a - b) < 0;
}

static inline bool is_idle(const Thread *t)
{
    for (uint32_t i = 0; i < g_cpu_count; i++)
        if (g_cpus[i].idle == t) return true;
    return false;
}

/* t's time on its core since it was last accounted: into vruntime; a new
   slice (deadline) once the current one is used up -- returns whether it
   was (time to pick again). */
static bool account(Thread *t, uint64_t now)
{
    if (!t || is_idle(t) || !t->weight) return false;
    uint64_t delta = now > t->exec_start ? now - t->exec_start : 0;
    t->exec_start = now;
    t->sum_exec += delta;
    t->vruntime += delta * NICE_0_WEIGHT / t->weight;
    if (!vbefore(t->vruntime, t->vdeadline)) {
        t->vdeadline = t->vruntime + vslice_of(t);
        return true;
    }
    return false;
}

/* V: the weighted average vruntime of the runnable threads -- queued, and
   running on some core */
static uint64_t avg_vruntime(void)
{
    uint64_t base = 0;
    bool have = false;
    int64_t sum = 0;
    uint64_t w = 0;
    for (Thread *t = g_rq; t; t = t->rq_next) {
        if (!have) { base = t->vruntime; have = true; }
        int64_t d = (int64_t)(t->vruntime - base);
        if (d > (1LL << 40)) d = 1LL << 40;
        if (d < -(1LL << 40)) d = -(1LL << 40);
        sum += d * (int64_t)t->weight;
        w += t->weight;
    }
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        Thread *t = g_cpus[i].current;
        if (!t || t == g_cpus[i].idle || t->state != THREAD_STATE_RUNNING || !t->weight) continue;
        if (!have) { base = t->vruntime; have = true; }
        int64_t d = (int64_t)(t->vruntime - base);
        if (d > (1LL << 40)) d = 1LL << 40;
        if (d < -(1LL << 40)) d = -(1LL << 40);
        sum += d * (int64_t)t->weight;
        w += t->weight;
    }
    if (!w) return g_v_last;
    g_v_last = base + (uint64_t)(sum / (int64_t)w);
    return g_v_last;
}

static void rq_add(Thread *t)
{
    if (t->on_rq) return;
    t->rq_prev = NULL;
    t->rq_next = g_rq;
    if (g_rq) g_rq->rq_prev = t;
    g_rq = t;
    t->on_rq = true;
    g_rq_count++;
}

static void rq_del(Thread *t)
{
    if (!t->on_rq) return;
    if (t->rq_prev) t->rq_prev->rq_next = t->rq_next;
    else g_rq = t->rq_next;
    if (t->rq_next) t->rq_next->rq_prev = t->rq_prev;
    t->rq_next = t->rq_prev = NULL;
    t->on_rq = false;
    g_rq_count--;
}

static void timer_add(Thread *t, uint64_t ms)
{
    t->timer_ms = ms;
    Thread **pp = &g_timers;
    while (*pp && (*pp)->timer_ms <= ms) pp = &(*pp)->tm_next;
    t->tm_next = *pp;
    *pp = t;
    t->on_timer = true;
}

static void timer_del(Thread *t)
{
    if (!t->on_timer) return;
    for (Thread **pp = &g_timers; *pp; pp = &(*pp)->tm_next)
        if (*pp == t) { *pp = t->tm_next; break; }
    t->tm_next = NULL;
    t->on_timer = false;
}

/* A thread joins the run queue: where it stands in virtual time.
   fresh: a new thread (no history: at V, half a slice). */
static void place(Thread *t, bool fresh)
{
    uint64_t V = avg_vruntime();
    uint64_t vs = vslice_of(t);
    if (fresh) {
        t->vruntime = V;
        t->vdeadline = V + vs / 2;
    } else {
        t->vruntime = V - (uint64_t)t->vlag;
        t->vdeadline = t->vruntime + vs;
    }
    t->vlag = 0;
}

/* BLOCKED (or new) -> READY, on the run queue */
static void make_ready(Thread *t, bool fresh)
{
    timer_del(t);
    place(t, fresh);
    t->state = THREAD_STATE_READY;
    rq_add(t);
}

/* Threads whose timed wait ran out: READY with -ETIMEDOUT */
static void timers_expire(void)
{
    extern uint64_t timer_get_ms(void);
    uint64_t now = timer_get_ms();
    while (g_timers && g_timers->timer_ms <= now) {
        Thread *t = g_timers;
        g_timers = t->tm_next;
        t->tm_next = NULL;
        t->on_timer = false;
        if (t->state == THREAD_STATE_BLOCKED) {
            t->wake_result = -110; /* -ETIMEDOUT */
            make_ready(t, false);
        }
    }
}

/* The eligible queued thread with the earliest deadline (or, if none is
   eligible, the one furthest behind). Dead ones leave the queue here. */
static Thread *pick_next(Thread *self, Thread *avoid)
{
    uint64_t V = avg_vruntime();
    Thread *best = NULL, *behind = NULL;
    bool avoided = false;
    for (Thread *t = g_rq, *nx; t; t = nx) {
        nx = t->rq_next;
        if (t->state != THREAD_STATE_READY) { rq_del(t); continue; }
        if (t->on_cpu && t != self) continue;
        if (t == avoid) { avoided = true; continue; }
        if (!vbefore(V, t->vruntime) && (!best || vbefore(t->vdeadline, best->vdeadline))) best = t;
        if (!behind || vbefore(t->vruntime, behind->vruntime)) behind = t;
    }
    if (best) return best;
    if (behind) return behind;
    return avoided ? avoid : NULL;
}

void sched_terminate(Thread *t)
{
    if (!t) return;
    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    if (t->state != THREAD_STATE_TERMINATED) {
        rq_del(t);
        timer_del(t);
        t->state = THREAD_STATE_TERMINATED;
        g_dead++;
        reaper_kick();
    }
    spin_unlock_irqrestore(&g_sched_lock, f);
}

void sched_set_nice(Thread *t, int nice)
{
    if (nice < -20) nice = -20;
    if (nice > 19) nice = 19;
    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    if (t->state == THREAD_STATE_RUNNING) account(t, sched_clock_ns());
    /* keep its place in virtual time; the slice ahead is in the new weight */
    t->nice = (int8_t)nice;
    t->weight = g_nice_weight[nice + 20];
    t->vdeadline = t->vruntime + vslice_of(t);
    spin_unlock_irqrestore(&g_sched_lock, f);
}

/* A thread starts its scheduling life (nice from its creator) */
static void sched_entity_init(Thread *t, const Thread *creator)
{
    int nice = creator && !is_idle(creator) ? creator->nice : 0;
    t->nice = (int8_t)nice;
    t->weight = g_nice_weight[nice + 20];
}

/* Each core's idle thread: runs whenever no thread is ready for this core
   (Phase 22a). It schedules (interrupts off; never in the big kernel
   lock), and when nothing is runnable sleeps in hlt until an interrupt --
   the 1 kHz tick, a wake-up IPI -- then looks again; each look's deadline
   pass also converts expired timed waits back to READY, so hlt sleeps
   cost at most one tick of wake latency. Never on the thread list: no
   other core may pick it. */
static void idle_loop(void *arg) {
    (void)arg;
    for (;;) {
        sched_schedule();
        __asm__ volatile("sti; hlt; cli" ::: "memory");
    }
}

/* An AP's start (kernel/smp.c ap_main()): it already runs on its idle
   thread's stack, without the lock */
void sched_idle_loop_ap(void) __attribute__((noreturn));
void sched_idle_loop_ap(void)
{
    __asm__ volatile("cli");
    idle_loop(NULL);
    for (;;) __asm__ volatile("hlt");
}

/* An idle thread. fresh: it starts through thread_trampoline at its first
   switch (the boot core's); otherwise the core is already running on its
   stack (an AP). */
static Thread *make_idle_thread(bool fresh)
{
    Thread *t = (Thread *)kmalloc(sizeof(Thread));
    if (!t) return NULL;
    memset(t, 0, sizeof(Thread));
    thread_fx_default(thread_fx_area(t));
    thread_fx_default(thread_fx_user(t));
    t->id = next_thread_id++;
    t->state = fresh ? THREAD_STATE_READY : THREAD_STATE_RUNNING;
    t->wake_deadline = SCHED_NO_DEADLINE;
    t->stack_orig = (uint64_t *)kmalloc(THREAD_STACK_SIZE);
    if (!t->stack_orig) { kfree(t); return NULL; }
    t->stack_orig[0] = KSTACK_GUARD;
    if (fresh) {
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
    }
    t->next = t;    /* not on the thread list */
    return t;
}

Thread *sched_new_ap_idle(void)
{
    return make_idle_thread(false);
}

void sched_init(void) {
    serial_write_string("Sched: Initializing scheduler...\r\n");
    Cpu *cpu = this_cpu();

    // Create the main thread structure representing the current running kernel main
    Thread *t0 = (Thread *)kmalloc(sizeof(Thread));
    /* Zero everything first: fields nobody sets explicitly (tls_base,
       clear_tid, ...) were heap garbage. tls_base goes straight into
       IA32_FS_BASE on every switch -- a non-canonical value is a #GP on
       real hardware / KVM (TCG never checked, so it went unnoticed). */
    memset(t0, 0, sizeof(Thread));
    thread_fx_default(thread_fx_user(t0));
    t0->id = 0;
    t0->rsp = 0; // Will be set on context switch
    t0->stack_orig = NULL; // Main stack is not dynamically allocated by us
    t0->state = THREAD_STATE_RUNNING;
    t0->proc = NULL; // Wired up by process_init(), called right after sched_init()
    t0->next = t0; // Circular linked list
    t0->wake_deadline = SCHED_NO_DEADLINE;
    t0->on_cpu = cpu->id + 1;
    t0->bkl = true;              /* the boot core holds the big lock from the start */
    sched_entity_init(t0, NULL);
    thread_list = t0;
    cpu->current = t0;
    cpu->fx_user = thread_fx_user(t0);

    /* Phase 22a: the idle thread. Without it, "all threads blocked" used to
       be a permanent cli;hlt halt inside sched_schedule() -- correct when
       nothing could ever unblock, fatal once real blocking exists (the
       waker needs a CPU to run on, and only the timer can provide one by
       preempting whoever is actually executing).

       Built by hand instead of via thread_create(): sched_init() runs
       BEFORE irq_init() (main.c boot order), and thread_create()'s trailing
       `sti` would enable interrupts before any IDT/PIC exists. Every other
       core gets its own (kernel/smp.c). */
    cpu->idle = make_idle_thread(true);

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
        memcpy(thread_fx_user(t), thread_fx_user(current_thread), FPU_AREA_MAX);
    else
        thread_fx_default(thread_fx_user(t));
    t->id = next_thread_id++;
    t->state = THREAD_STATE_BLOCKED;   /* READY below, once it is complete */
    t->tls_base = tls_base;
    t->clear_tid = NULL;
    t->sig_pending = 0;
    t->sig_mask = 0;
    t->sig_frame = NULL;
    t->sig_restore_mask = false;
    t->proc = proc ? proc : current_thread->proc;
    /* starts inside the big kernel lock, like its creator: it gives it
       back on its way to user mode (sched_finish() takes it first) */
    t->bkl = true;

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

    // Insert into the circular list (after its head: the creator may be
    // an idle thread, which is on no list)
    t->next = thread_list->next;
    thread_list->next = t;

    {
        spin_lock(&g_sched_lock);
        sched_entity_init(t, current_thread);
        make_ready(t, true);
        spin_unlock(&g_sched_lock);
    }

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

    sched_terminate(current_thread);
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

/* ---- dead processes: torn down by a thread of their own ----------------
   process_teardown() closes the process's files and sockets -- a TCP close
   waits for its FIN to go out, a disk flush for the disk -- and the
   scheduler must never sleep: a teardown that blocked inside
   schedule_core() switched threads from within the switch, and the
   interrupted scheduler later went on with the list pointers and the core
   it had before (on SMP, the wrong core's data: another core's kernel
   stack in this one's TSS). The scheduler queues the process; the reaper
   thread tears it down where sleeping is fine. */
#define REAP_MAX 256
static struct Process *g_reap[REAP_MAX];
static uint32_t g_reap_head, g_reap_tail;
static Thread *g_reaper;

static void reap_queue(struct Process *p)
{
    uint32_t next = (g_reap_head + 1) % REAP_MAX;
    if (next == g_reap_tail) {              /* full: hold on to it a while */
        serial_write_string("Sched: reaper queue full, a process leaks\r\n");
        return;
    }
    g_reap[g_reap_head] = p;
    g_reap_head = next;
}

/* A thread died (g_sched_lock held): the reaper frees it. Only from its
   own wait: asleep inside a teardown (a socket close) it is on another
   queue, and looks again afterwards. */
static void reaper_kick(void)
{
    if (g_reaper && g_reaper->state == THREAD_STATE_BLOCKED && g_reaper->wq == (void *)g_reap) {
        g_reaper->wake_result = 0;
        g_reaper->wq = NULL;
        make_ready(g_reaper, false);
    }
}

/* Free the terminated threads no core runs any more (the big lock held:
   the thread list is its). on_cpu is read under the scheduler's lock: a
   thread switched away is off its stack only once the switch is done. */
static void sweep_dead(void)
{
    if (!g_dead || !thread_list) return;
    Thread *prev = thread_list;
    Thread *curr = thread_list->next;
    do {
        bool gone = false;
        if (curr->state == THREAD_STATE_TERMINATED) {
            spin_lock(&g_sched_lock);
            gone = !curr->on_cpu;
            spin_unlock(&g_sched_lock);
        }
        if (gone) {
            prev->next = curr->next;
            if (curr == thread_list) {
                thread_list = curr->next;
            }
            Thread *to_free = curr;
            __atomic_sub_fetch(&g_dead, 1, __ATOMIC_RELAXED);
            curr = curr->next;
            /* killed while blocked (signal_kill_process(), a fatal
               signal): it never ran its own unlink, so its wait queue
               would keep pointing at the freed Thread */
            if (to_free->wq_head) {
                /* a futex queue is under the futex lock, a wait queue
                   under the wait queues' (futex -> waitqueue: only here) */
                uint64_t ff = futex_lock_irqsave();
                uint64_t wf = waitqueue_lock_irqsave();
                Thread **pp = to_free->wq_head;
                while (*pp) {
                    if (*pp == to_free) { *pp = to_free->wq_next; break; }
                    pp = &(*pp)->wq_next;
                }
                to_free->wq_head = NULL;
                waitqueue_unlock_irqrestore(wf);
                futex_unlock_irqrestore(ff);
            }

            /* The address space goes once every thread sharing this
               process (SYS_clone pthreads included, see kernel/syscall.c)
               has terminated and left its core -- no core still walks its
               page tables, and no surviving pthread sibling is left
               running on a freed address space. process_teardown() itself
               refuses to touch the kernel process (pid 0). */
            if (to_free->proc) {
                to_free->proc->thread_count--;
                if (to_free->proc->exited && to_free->proc->thread_count <= 0) {
                    reap_queue(to_free->proc);
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

static void reaper_thread(void *arg)
{
    (void)arg;
    extern uint64_t timer_get_ms(void);
    for (;;) {
        __asm__ volatile("cli");
        sweep_dead();
        while (g_reap_tail != g_reap_head) {
            struct Process *p = g_reap[g_reap_tail];
            g_reap_tail = (g_reap_tail + 1) % REAP_MAX;
            process_teardown(p);
        }
        /* a dead thread still on its core (its last moments in
           thread_exit()): looked at again shortly */
        if (g_dead) sched_block_ms(&g_reap, timer_get_ms() + 5);
        else sched_block(&g_reap, SCHED_NO_DEADLINE);   /* sched_terminate() wakes it */
    }
}

void sched_start_reaper(void)
{
    g_reaper = thread_create(reaper_thread, NULL);
}

/* After a switch, in the thread switched to (or with none, in the one
   that goes on): the scheduler's lock goes, and the big kernel lock comes
   back if the thread was inside it. Interrupts still off. */
static void sched_finish(void)
{
    spin_unlock(&g_sched_lock);
    Thread *t = current_thread;
    if (t->bkl && g_bkl_owner != (int32_t)this_cpu()->id) bkl_lock();
}

/* A new thread's first instructions (kernel/sched_switch.asm) */
void sched_thread_start(void)
{
    sched_finish();
}

/* Pick the next thread for this core and switch to it. g_sched_lock held
   (interrupts off), released on the way out (sched_finish()). Returns
   whether it switched (and came back). Never sleeps (see the reaper
   above). yielding: the current thread goes on only if no other one can
   (kernel code spins on sched_yield() waiting for another thread -- that
   one must get the core even when it is not eligible yet). */
static bool schedule_locked(bool yielding) {
    Cpu *cpu = this_cpu();
    /* timed waits that ran out: READY with -ETIMEDOUT (the timer list is
       sorted: only the expired ones are looked at) */
    timers_expire();

    Thread *prev_thread = cpu->current;
    Thread *next_thread = NULL;
    uint64_t now = sched_clock_ns();

    /* The thread leaving the core, if it can go on, competes like the
       others: back on the run queue with its vruntime so far */
    bool prev_runnable = prev_thread != cpu->idle &&
        (prev_thread->state == THREAD_STATE_RUNNING || prev_thread->state == THREAD_STATE_READY);
    if (prev_thread != cpu->idle) account(prev_thread, now);
    if (prev_runnable) {
        prev_thread->state = THREAD_STATE_READY;
        rq_add(prev_thread);
    }

    next_thread = pick_next(prev_thread, yielding ? prev_thread : NULL);
    if (next_thread) rq_del(next_thread);

    if (next_thread == prev_thread) {
        prev_thread->state = THREAD_STATE_RUNNING;   /* it goes on */
        sched_finish();
        return false;
    }
    if (!next_thread) {
        /* Phase 22a: everything real is BLOCKED. Hand the core to its idle
           thread instead of halting forever. The blocked thread we came
           from keeps its parked state untouched; it resumes inside its own
           sched_block() frame whenever something marks it READY. */
        if (prev_thread == cpu->idle) {
            prev_thread->state = THREAD_STATE_RUNNING;
            sched_finish();
            return false;
        }
        next_thread = cpu->idle;
    }

    /* the big lock is not the next thread's: it goes back now (accounted
       to the thread leaving) and the next one takes it if it needs it */
    bkl_switch_out();

    // Switch to next_thread
    next_thread->state = THREAD_STATE_RUNNING;
    next_thread->exec_start = now;
    if (next_thread->on_cpu) {
        serial_write_string("SCHED: picked a thread another core runs\r\n");
    }
    /* No other core can take prev_thread before this switch is done: they
       all need g_sched_lock, held until the next thread runs. */
    prev_thread->on_cpu = 0;
    next_thread->on_cpu = cpu->id + 1;
    cpu->current = next_thread;

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

    /* The new thread's address space (a kernel thread, an idle thread:
       the kernel's own tables -- a dead process's must not stay loaded on
       some core while its pages are freed). Only on a change: a reload
       flushes the TLB. */
    {
        PageTable *want = (next_thread->proc && next_thread->proc->pml4)
                          ? next_thread->proc->pml4 : vmm_get_kernel_pml4();
        if (cpu->cr3 != V2P(want)) {
            cpu->cr3 = V2P(want);
            __asm__ volatile("mov %0, %%cr3" :: "r"(cpu->cr3) : "memory");
        }
    }

    /* Per-thread x87/SSE state at the switch point (kernel code uses SSE
       too). Without this every thread shared one register file: a
       preemption in the middle of an SSE memcpy handed the next thread's
       values to the first. User state is separate (fx_user, saved/restored
       by the entry stubs). No SSE is used between here and the stack
       switch. */
    if (prev_thread != next_thread) {
        fpu_save(thread_fx_area(prev_thread));
        fpu_restore(thread_fx_area(next_thread));
        cpu->fx_user = thread_fx_user(next_thread);
    }
    context_switch(&prev_thread->rsp, next_thread->rsp);
    sched_finish();
    return true;
}

/* The big lock's deferred interrupt work first (it may wake threads: the
   scheduler's lock is not held yet), then the scheduler's lock. */
static void sched_lock_for_switch(void)
{
    if (g_deferred_due && g_bkl_owner == (int32_t)this_cpu()->id) smp_run_deferred();
    spin_lock(&g_sched_lock);
}

static bool schedule_core(void)
{
    sched_lock_for_switch();
    return schedule_locked(false);
}

void sched_schedule(void) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));
    schedule_core();
    if (rflags & 0x200) __asm__ volatile("sti");
}


void sched_yield(void) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

    /* EEVDF's yield: the rest of this slice goes to whoever else is
       eligible -- the thread's deadline moves one slice on */
    Thread *me = current_thread;
    sched_lock_for_switch();
    if (me != this_cpu()->idle && me->weight) {
        account(me, sched_clock_ns());
        me->vdeadline += vslice_of(me);
    }
    /* Nothing else to run here: other cores may be waiting for the kernel
       lock (the very thread this one waits for, perhaps) */
    if (!schedule_locked(true)) bkl_relax();

    if (rflags & 0x200) {
        __asm__ volatile("sti");
    }
}

/* While > 0 the timer does not switch threads (interrupts may be on):
   for a thread that must not be interleaved with others but should not
   hold interrupts off for long either -- a disk command it waits for. */
volatile int g_sched_no_preempt;

/* Each core's 1 kHz tick: the running thread is accounted, and the core
   picks again when its slice is used up, or when a queued thread is
   eligible with an earlier deadline (one that just woke up) */
void sched_preempt_tick(void) {
    if (g_sched_no_preempt) return;
    Cpu *cpu = this_cpu();
    Thread *t = cpu->current;
    if (!t || t == cpu->idle) return;
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));
    spin_lock(&g_sched_lock);
    bool again = false;
    if (t->state == THREAD_STATE_TERMINATED) {
        again = true;             /* killed by another core while running here */
    } else if (t->state == THREAD_STATE_RUNNING) {
        if (account(t, sched_clock_ns())) again = true;      /* slice used up */
        else if (g_rq) {
            Thread *n = pick_next(NULL, NULL);
            if (n && vbefore(n->vdeadline, t->vdeadline)) again = true;
        }
        if (g_timers) {
            extern uint64_t timer_get_ms(void);
            if (g_timers->timer_ms <= timer_get_ms()) again = true;   /* a timed wait ran out */
        }
    }
    if (again) schedule_locked(false);
    else spin_unlock(&g_sched_lock);
    if (rflags & 0x200) __asm__ volatile("sti");
}

Thread *sched_current(void) {
    return current_thread;
}

/* ---- Phase 22a: real blocking primitives ---- */

/* A signal that will be delivered (unblocked, or SIGKILL) is pending */
static inline bool signal_waiting(const Thread *t)
{
    return (__atomic_load_n(&t->sig_pending, __ATOMIC_RELAXED) & (~t->sig_mask | (1ULL << 9))) != 0;
}

/* intr: not at all when a signal is waiting -- looked at under the lock, so
   a sender (which sets sig_pending, then looks at the state) either sees
   the thread BLOCKED and wakes it, or the thread sees the signal */
/* drop: a spinlock the caller holds (with its wait queue): let go once the
   thread is BLOCKED -- a waker that takes it after that finds the thread
   asleep (and waits for the scheduler's lock until the switch is done) */
static int64_t sched_block_until(void *wq, uint64_t wake_deadline, uint64_t wake_deadline_ms, bool intr,
                                 Spinlock *drop) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

    Thread *t = current_thread;
    /* Its process is dying (exit_group(), a fatal signal, killed by
       another core while this thread ran there): do not park. Its killer
       woke the threads that were blocked then; one running on another
       core that blocks only now would sleep for good -- the process never
       torn down, its pipes and files never closed. And BLOCKED would
       bring a TERMINATED thread back to life. */
    if (sched_dying()) {
        if (drop) spin_unlock(drop);
        if (rflags & 0x200) __asm__ volatile("sti");
        return -4; /* -EINTR */
    }
    /* the big lock's deferred interrupt work wakes wait queues: not with
       the caller's lock held (it may be the wait queues' own) -- left to
       the lock's next holder or the next tick */
    if (drop) spin_lock(&g_sched_lock);
    else sched_lock_for_switch();
    if (t->state == THREAD_STATE_TERMINATED || (intr && signal_waiting(t))) {   /* killed meanwhile (another core) */
        spin_unlock(&g_sched_lock);
        if (drop) spin_unlock(drop);
        if (rflags & 0x200) __asm__ volatile("sti");
        return -4; /* -EINTR */
    }
    t->wq = wq;
    t->wake_deadline = wake_deadline;
    t->wake_deadline_ms = wake_deadline_ms;
    /* how far behind (or ahead of) the others it was: it comes back there */
    if (t->weight) {
        account(t, sched_clock_ns());
        int64_t lag = (int64_t)(avg_vruntime() - t->vruntime);
        int64_t lim = (int64_t)(2 * vslice_of(t));
        if (lag > lim) lag = lim;
        if (lag < -lim) lag = -lim;
        t->vlag = lag;
    }
    /* a timeout: on the timer list, by the 1 kHz clock (wake_deadline
       counts the 100 Hz ticks) */
    {
        extern uint64_t timer_get_ticks(void);
        extern uint64_t timer_get_ms(void);
        uint64_t now_ms = timer_get_ms();
        uint64_t when = SCHED_NO_DEADLINE;
        if (wake_deadline != SCHED_NO_DEADLINE) {
            uint64_t now_t = timer_get_ticks();
            when = now_ms + (wake_deadline > now_t ? (wake_deadline - now_t) * 10 : 0);
        }
        if (wake_deadline_ms != SCHED_NO_DEADLINE && (when == SCHED_NO_DEADLINE || wake_deadline_ms < when))
            when = wake_deadline_ms;
        if (when != SCHED_NO_DEADLINE) timer_add(t, when);
    }
    /* wake_result is written by sched_unblock()/the deadline pass BEFORE
       this thread is marked READY, so no initialization race exists: we
       cannot be scheduled until state leaves BLOCKED. */
    t->state = THREAD_STATE_BLOCKED;
    if (drop) spin_unlock(drop);

    schedule_locked(false);
    /* Resumed: either a waker or the deadline pass set our result and made
       us READY. Restore the caller's interrupt flag exactly like
       sched_yield() does. */
    if (rflags & 0x200) {
        __asm__ volatile("sti");
    }
    return t->wake_result;
}

int64_t sched_block(void *wq, uint64_t wake_deadline) {
    return sched_block_until(wq, wake_deadline, SCHED_NO_DEADLINE, false, NULL);
}

int64_t sched_block_ms(void *wq, uint64_t wake_deadline_ms) {
    return sched_block_until(wq, SCHED_NO_DEADLINE, wake_deadline_ms, false, NULL);
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
    sched_block_until(NULL, SCHED_NO_DEADLINE, timer_get_ms() + ms, false, NULL);
}

int64_t sched_block_unlock(void *wq, uint64_t wake_deadline, Spinlock *l) {
    return sched_block_until(wq, wake_deadline, SCHED_NO_DEADLINE, true, l);
}

int64_t sched_block_unlock_ex(void *wq, uint64_t wake_deadline, uint64_t wake_deadline_ms,
                              Spinlock *l, bool intr) {
    return sched_block_until(wq, wake_deadline, wake_deadline_ms, intr, l);
}

int64_t sched_sleep_ms_intr(uint64_t ms) {
    extern uint64_t timer_get_ms(void);
    if (ms == 0) {
        sched_yield();
        return 0;
    }
    return sched_block_until(NULL, SCHED_NO_DEADLINE, timer_get_ms() + ms, true, NULL);
}

void sched_unblock(Thread *t, int64_t result) {
    if (!t) return;
    uint64_t rflags = spin_lock_irqsave(&g_sched_lock);
    if (t->state == THREAD_STATE_BLOCKED) {
        t->wake_result = result;
        t->wq = NULL;
        make_ready(t, false);
        smp_kick_idle();     /* an idle core may run it right away */
    }
    spin_unlock_irqrestore(&g_sched_lock, rflags);
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

/* ---- debugging: what are the user threads doing? ---------------------- */

static bool dump_read_u64(PageTable *pml4, uint64_t va, uint64_t *out)
{
    if ((va & 7) || va >= 0x0000800000000000ULL || !vmm_is_user_page(pml4, va)) return false;
    uint64_t phys = vmm_get_phys(pml4, va);
    if (!phys) return false;
    *out = *(volatile uint64_t *)P2V(phys);
    return true;
}

void sched_dump_user_threads(void)
{
    extern void uint_to_str(uint64_t val, char *buf);
    static const char *states[] = { "ready", "running", "blocked", "terminated" };
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));
    {
        extern uint64_t pmm_get_free_memory(void);
        char b[32];
        serial_write_string("==== user threads ==== free ");
        uint_to_str(pmm_get_free_memory() >> 20, b); serial_write_string(b);
        serial_write_string(" MB, page cache ");
        uint_to_str((uint64_t)pcache_pages() * 4 / 1024, b); serial_write_string(b);
        serial_write_string(" MB\r\n");
    }
    Thread *t = thread_list;
    int guard = 0;
    if (t) do {
        if (t->stack_orig && t->state != THREAD_STATE_TERMINATED && (!t->proc || t->proc->pid == 0)) {
            /* a kernel thread: what it waits on */
            char buf[32];
            serial_write_string("kernel tid ");
            uint_to_str(t->id, buf); serial_write_string(buf);
            serial_write_string(t == g_reaper ? " (reaper, " : " (");
            uint_to_str((g_reap_head + REAP_MAX - g_reap_tail) % REAP_MAX, buf); serial_write_string(buf);
            serial_write_string(" queued) ");
            serial_write_string(states[t->state <= THREAD_STATE_TERMINATED ? t->state : 0]);
            serial_write_string(t->wq == (void *)g_reap ? " on its queue\r\n"
                                : (t->wq ? " on a wait queue\r\n" : "\r\n"));
        }
        if (t->proc && t->proc->pid != 0 && t->state != THREAD_STATE_TERMINATED && t->stack_orig) {
            char buf[32];
            serial_write_string("tid ");
            uint_to_str(t->id, buf); serial_write_string(buf);
            serial_write_string(" pid ");
            uint_to_str(t->proc->pid, buf); serial_write_string(buf);
            serial_write_string(" ");
            serial_write_string(t->proc->exe_path);
            serial_write_string(" ");
            serial_write_string(states[t->state <= THREAD_STATE_TERMINATED ? t->state : 0]);
            serial_write_string(" syscall ");
            uint_to_str(t->last_syscall, buf); serial_write_string(buf);
            if (t == t->proc->main_thread) {
                /* the process's time: ms in user code / in the kernel, page
                   faults and syscalls with their cost (Mcycles) */
                Process *pp = t->proc;
                serial_write_string("\r\n  time: user ");
                uint_to_str(pp->st_user_ticks, buf); serial_write_string(buf);
                serial_write_string(" ms, kernel ");
                uint_to_str(pp->st_kernel_ticks, buf); serial_write_string(buf);
                serial_write_string(" ms; faults ");
                uint_to_str(pp->st_faults, buf); serial_write_string(buf);
                serial_write_string(" (");
                uint_to_str(pp->st_fault_tsc / 1000000, buf); serial_write_string(buf);
                serial_write_string(" Mcyc); syscalls ");
                uint_to_str(pp->st_syscalls, buf); serial_write_string(buf);
                serial_write_string(" (");
                uint_to_str(pp->st_sys_tsc / 1000000, buf); serial_write_string(buf);
                serial_write_string(" Mcyc); vmas ");
                uint64_t nv = 0;
                for (VMA *v = pp->vma_list; v; v = v->next) nv++;
                uint_to_str(nv, buf); serial_write_string(buf);
            }
            /* the user context at the top of its kernel stack: an interrupt
               frame from ring 3 (SS 0x1B, CS 0x23) or syscall_entry's */
            uint64_t *top = (uint64_t *)((((uint64_t)t->stack_orig + THREAD_STACK_SIZE) & ~0xFULL));
            uint64_t rip, rsp;
            if (top[-1] == 0x1B && top[-4] == 0x23) { rip = top[-5]; rsp = top[-2]; serial_write_string(" (preempted)"); }
            else                                     { rip = top[-4]; rsp = top[-2]; }
            serial_write_string("\r\n  rip ");
            if (!vma_print_addr(t->proc, rip)) serial_write_string("?");
            serial_write_string("\r\n");
            int shown = 0;
            for (int k = 0; k < 4096 && shown < 14; k++) {
                uint64_t w;
                if (!dump_read_u64(t->proc->pml4, rsp + 8 * (uint64_t)k, &w)) break;
                VMA *v = vma_find(t->proc, w);
                if (!v || !(v->prot & VMA_PROT_EXEC) || !(v->flags & VMA_FILE)) continue;
                serial_write_string("  stack ");
                vma_print_addr(t->proc, w);
                serial_write_string("\r\n");
                shown++;
            }
        }
        t = t->next;
    } while (t != thread_list && ++guard < 10000);
    extern void process_dump_unreaped(void);
    process_dump_unreaped();
    extern void bkl_profile_dump(void);
    bkl_profile_dump();
    serial_write_string("==== end ====\r\n");
    if (rflags & 0x200) __asm__ volatile("sti");
}

bool sched_dying(void)
{
    Thread *t = current_thread;
    return t && t->proc && t->proc->pid != 0 &&
           ((t->sig_pending & (1ULL << 9)) || t->proc->exited ||
            t->state == THREAD_STATE_TERMINATED);
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
                sched_terminate(t);
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

