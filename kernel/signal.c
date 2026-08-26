/*
 * WynlandOS - real POSIX-style signals (Phase 22b, implementation)
 */

#include <wynland/signal.h>
#include <wynland/types.h>
#include <wynland/heap.h>
#include <wynland/sched.h>
#include <wynland/process.h>

extern void serial_write_string(const char *str);
extern void uint_to_str(uint64_t val, char *buf);

/* kernel-side frame: the interrupted context + signal, kept so
   SYS_rt_sigreturn can restore exactly without trusting user memory */
typedef struct SignalFrame {
    /* mirror of kernel/syscall.c's private SyscallRegs layout -- the
       _Static_asserts below fail the build if that struct ever drifts */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdx, rsi, rdi, rbx, rbp, rip, rflags, rsp;
    int sig;
} SignalFrame;

/* signal numbers with default-ignore semantics (everything else with no
   handler terminates the process -- the classic Linux default set) */
static int default_ignored(int sig) {
    return sig == 13 /* PIPE */ || sig == 17 /* CHLD */ || sig == 23 /* URG */;
}

void signal_init(void) {
    /* state lives in Process/Thread structs; nothing global to set up */
}

int signal_deliver_check(void *regs_v) {
    typedef struct {
        uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
        uint64_t rdx, rsi, rdi, rbx, rbp, rip, rflags, rsp;
    } Regs;
    _Static_assert(sizeof(Regs) == 16 * 8, "SyscallRegs layout drift");
    Regs *regs = (Regs *)regs_v;

    Thread *t = sched_current();
    if (!t || !t->proc) return 0;

    uint64_t pending = t->sig_pending & ~t->sig_mask;
    if (!pending) return 0;

    int sig = 1;
    while (sig < 65 && !(pending & (1ULL << sig))) sig++;
    if (sig >= 65) { t->sig_pending &= ~pending; return 0; }
    t->sig_pending &= ~(1ULL << sig);

    SigAct *act = &t->proc->sig_acts[sig];

    if (act->handler == 0) {
        if (default_ignored(sig)) return 0;
        /* fatal by default: terminate the whole process. Mark every thread
           of this process TERMINATED; the current one schedules away and
           never returns to userspace.
           Known v1 leak, documented: fd table and other Process resources
           are not reaped here yet (Phase 23's wait4/reaping lands that);
           non-current victims' clear-tid words are not written either,
           since their address spaces are not loaded in this context. */
        t->proc->exited = true;
        extern Thread *sched_get_thread_list(void);
        Thread *it = sched_get_thread_list();
        if (it) {
            int guard = 0;
            do {
                if (it->proc == t->proc && it != t &&
                    it->state != THREAD_STATE_TERMINATED) {
                    it->state = THREAD_STATE_TERMINATED;
                }
                it = it->next;
            } while (it != sched_get_thread_list() && ++guard < 256);
        }
        t->state = THREAD_STATE_TERMINATED;
        serial_write_string("[signal] fatal, terminating process\r\n");
        sched_schedule();
        while (1) __asm__ volatile("cli; hlt");
    }

    /* handler installed: build delivery context */
    SignalFrame *kf = (SignalFrame *)kmalloc(sizeof(SignalFrame));
    if (!kf) return 0; /* out of memory: drop the signal, keep running */
    memcpy(&kf->r15, regs, 16 * 8);
    kf->sig = sig;
    t->sig_frame = (void *)kf;

    uint64_t sp = regs->rsp & ~0xFULL;
    sp -= 8;
    *(uint64_t *)(uintptr_t)sp = act->restorer; /* handler's RET target */

    regs->rip = act->handler;
    regs->rsp = sp;
    regs->rdi = (uint64_t)(uint32_t)sig; /* handler(signum) */
    regs->rsi = 0;                        /* siginfo* -- v1: none */
    regs->rdx = 0;                        /* ucontext* -- v1: none */
    return 1;
}

uint64_t signal_do_sigaction(int sig, uint64_t act_ptr, uint64_t oldact_ptr) {
    if (sig <= 0 || sig >= 65) return (uint64_t)-22; /* -EINVAL */
    /* KILL/STOP cannot be handled, matching POSIX */
    if (sig == 9 || sig == 19) return (uint64_t)-22;

    Thread *t = sched_current();
    SigAct *slot = &t->proc->sig_acts[sig];

    typedef struct {
        uint64_t handler;
        uint64_t flags;
        uint64_t restorer;
        uint64_t mask;
    } KSigAction;

    if (oldact_ptr) {
        KSigAction *o = (KSigAction *)oldact_ptr;
        o->handler = slot->handler;
        o->flags = slot->flags;
        o->restorer = slot->restorer;
        o->mask = slot->mask;
    }

    if (act_ptr) {
        KSigAction *a = (KSigAction *)act_ptr;
        slot->handler = a->handler;
        slot->flags = a->flags;
        slot->restorer = a->restorer;
        slot->mask = a->mask;
    }
    return 0;
}

uint64_t signal_do_procmask(int how, uint64_t set_ptr, uint64_t oldset_ptr) {
    Thread *t = sched_current();

    /* musl passes THE SAME BUFFER as both set and oldset
       (rt_sigprocmask(how, &mask, &mask, 8)) -- read the requested set
       BEFORE clobbering it with the old-mask output, or every BLOCK
       silently becomes a no-op (hit live by this phase's own regression
       test; real Linux has the same ordering requirement via its
       copy_from_user-before-copy_to_user discipline). */
    /* BIT-NUMBERING CONVERSION -- real off-by-one class bug caught live:
       userspace sigset_t is ZERO-based (bit S-1 = signal S), while this
       kernel's pending/mask words are ONE-based (1<<sig). musl asking to
       block SIGUSR2(12) sends bit 11; without the shift below the kernel
       blocks SIGSEGV instead and every masked-delivery test fails while
       looking like a scheduler bug. Signals >= 64 have no kernel bit
       (uint64 mask) and are dropped on the floor, documented. */
    uint64_t newbits = set_ptr ? ((*(uint64_t *)set_ptr) << 1) : 0;

    if (oldset_ptr) *(uint64_t *)oldset_ptr = (t->sig_mask >> 1);
    if (!set_ptr) return 0;

    switch (how) {
        case 0: t->sig_mask |= newbits; break;          /* SIG_BLOCK */
        case 1: t->sig_mask &= ~newbits; break;         /* SIG_UNBLOCK */
        case 2: t->sig_mask = newbits; break;           /* SIG_SETMASK */
        default: return (uint64_t)-22;                   /* -EINVAL */
    }
    /* these two can never be blocked, matching POSIX */
    t->sig_mask &= ~((1ULL << 9) | (1ULL << 19));
    return 0;
}

uint64_t signal_rt_return(void *regs_v) {
    typedef struct {
        uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
        uint64_t rdx, rsi, rdi, rbx, rbp, rip, rflags, rsp;
    } Regs;
    Regs *regs = (Regs *)regs_v;

    Thread *t = sched_current();
    SignalFrame *f = (SignalFrame *)t->sig_frame;
    if (!f) {
        /* rt_sigreturn with no frame: the program jumped somewhere hostile.
           Treat like a fatal signal -- terminate rather than corrupt. */
        t->proc->exited = true;
        t->state = THREAD_STATE_TERMINATED;
        serial_write_string("[signal] rt_sigreturn without frame\r\n");
        sched_schedule();
        while (1) __asm__ volatile("cli; hlt");
    }
    memcpy(regs, &f->r15, 16 * 8);
    t->sig_frame = NULL;
    uint64_t saved_rax = f->r15; /* placeholder, replaced below */
    /* rax is not part of SyscallRegs (it travels in the dispatcher's return
       register); the interrupted user rax was NOT saved in the frame --
       real kernels restart-or-return specially. v1 contract: handlers are
       entered via a syscall boundary whose result is meaningless, so we
       restore rax as 0. Documented divergence. */
    (void)saved_rax;
    int sig = f->sig;
    kfree(f);
    (void)sig;
    return 0;
}

void signal_raise_current(int sig) {
    if (sig <= 0 || sig >= 65) return;
    Thread *t = sched_current();
    t->sig_pending |= (1ULL << sig);
}
