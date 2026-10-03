/*
 * WynlandOS - real POSIX-style signals (Phase 22b, implementation)
 */

#include <wynland/signal.h>
#include <wynland/types.h>
#include <wynland/heap.h>
#include <wynland/sched.h>
#include <wynland/vma.h>
#include <wynland/process.h>
#include <wynland/usercopy.h>

/* first non-user address: canonical low half ends here */
#define SIG_USER_LIMIT 0x0000800000000000ULL

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
    uint64_t rax; /* the interrupted syscall's result: handed back by
                     rt_sigreturn so the code after the handler sees it */
    uint8_t fx[FPU_AREA_MAX]; /* the interrupted code's FPU state: the handler may
                        use those registers; rt_sigreturn puts it back */
} SignalFrame;

/* signal numbers with default-ignore semantics (everything else with no
   handler terminates the process -- the classic Linux default set) */
static int default_ignored(int sig) {
    return sig == 13 /* PIPE */ || sig == 17 /* CHLD */ || sig == 23 /* URG */;
}

/* Kill a whole process with `sig` (its wait status says so): every one
   of its threads is marked TERMINATED, the current one included if it
   belongs to it -- the caller schedules away afterwards. */
void signal_kill_process(Process *p, int sig) {
    if (!p) return;
    process_mark_exited(p, sig & 0x7F);
    extern Thread *sched_get_thread_list(void);
    Thread *it = sched_get_thread_list();
    if (it) {
        int guard = 0;
        do {
            if (it->proc == p && it->state != THREAD_STATE_TERMINATED)
                it->state = THREAD_STATE_TERMINATED;
            it = it->next;
        } while (it != sched_get_thread_list() && ++guard < 4096);
    }
}

void signal_init(void) {
    /* state lives in Process/Thread structs; nothing global to set up */
}

int signal_deliver_check(void *regs_v, uint64_t sysret) {
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

    if (act->handler == 1) return 0; /* SIG_IGN: used to be "called" at address 1 */
    if (act->handler == 0) {
        if (default_ignored(sig)) return 0;
        /* fatal by default: terminate the whole process. Mark every thread
           of this process TERMINATED; the current one schedules away and
           never returns to userspace.
           Known v1 leak, documented: fd table and other Process resources
           are not reaped here yet (Phase 23's wait4/reaping lands that);
           non-current victims' clear-tid words are not written either,
           since their address spaces are not loaded in this context. */
        signal_kill_process(t->proc, sig);
        {
            char m[] = "[signal] fatal sig=00, terminating process, rip=0x0000000000000000\r\n";
            m[19] = (char)('0' + sig / 10);
            m[20] = (char)('0' + sig % 10);
            for (int k = 0; k < 16; k++)
                m[50 + k] = "0123456789abcdef"[(regs->rip >> (60 - 4 * k)) & 0xF];
            serial_write_string(m);
            /* who, and where: the file under RIP and the probable callers */
            serial_write_string("  in ");
            serial_write_string(t->proc->exe_path);
            serial_write_string("\r\n  rip: ");
            if (!vma_print_addr(t->proc, regs->rip)) serial_write_string("?");
            serial_write_string("\r\n");
            vma_print_stack(t->proc, regs->rsp);
        }
        sched_schedule();
        while (1) __asm__ volatile("cli; hlt");
    }

    /* never sysret to a non-user RIP (sigaction checks it too) */
    if (act->handler >= SIG_USER_LIMIT || act->restorer >= SIG_USER_LIMIT) {
        process_mark_exited(t->proc, 11);
        t->state = THREAD_STATE_TERMINATED;
        sched_schedule();
        while (1) __asm__ volatile("cli; hlt");
    }

    /* handler installed: build delivery context */
    SignalFrame *kf = (SignalFrame *)kmalloc(sizeof(SignalFrame));
    if (!kf) return 0; /* out of memory: drop the signal, keep running */
    memcpy(&kf->r15, regs, 16 * 8);
    kf->sig = sig;
    kf->rax = sysret;
    memcpy(kf->fx, thread_fx_user(t), FPU_AREA_MAX);
    t->sig_frame = (void *)kf;

    /* Skip the 128-byte red zone below the interrupted RSP first (SysV
       x86-64 ABI: leaf code keeps live locals there without moving RSP).
       Writing the return address straight under RSP clobbered them --
       glibc saw it as "*** stack smashing detected ***" when a SIGCHLD
       handler ran on the way back from wait4(). */
    uint64_t sp = (regs->rsp - 128) & ~0xFULL;
    sp -= 8; /* handler entry: RSP = 8 mod 16, as after a CALL */
    if (!user_prepare_write(sp, sizeof(uint64_t))) {
        /* no usable user stack for the handler: like Linux, the process
           dies of SIGSEGV instead of the kernel faulting on the store */
        t->sig_frame = NULL;
        kfree(kf);
        process_mark_exited(t->proc, 11);
        t->state = THREAD_STATE_TERMINATED;
        sched_schedule();
        while (1) __asm__ volatile("cli; hlt");
    }
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

    /* User pointers go through copy_from_user/copy_to_user: they used to be
       dereferenced raw, i.e. any process could read (act) or write (oldact)
       kernel memory through this call. The new action is read BEFORE the
       old one is written -- act and oldact may be the same buffer. */
    KSigAction na;
    if (act_ptr && copy_from_user(&na, (const void *)act_ptr, sizeof(na)) != 0)
        return (uint64_t)-14; /* -EFAULT */
    if (oldact_ptr) {
        KSigAction o = { slot->handler, slot->flags, slot->restorer, slot->mask };
        if (copy_to_user((void *)oldact_ptr, &o, sizeof(o)) != 0) return (uint64_t)-14;
    }
    /* handler and restorer end up in RIP via sysret: a non-canonical
       address there faults in ring 0 with the user stack already loaded
       (the CVE-2012-0217 shape). 0/1 are SIG_DFL/SIG_IGN. */
    if (act_ptr && ((na.handler > 1 && na.handler >= SIG_USER_LIMIT) ||
                    (na.restorer && na.restorer >= SIG_USER_LIMIT)))
        return (uint64_t)-22; /* -EINVAL */
    if (act_ptr) {
        slot->handler = na.handler;
        slot->flags = na.flags;
        slot->restorer = na.restorer;
        slot->mask = na.mask;
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
    /* copy_from_user/copy_to_user: both pointers used to be dereferenced
       raw (arbitrary kernel read/write from user space). */
    uint64_t uset = 0;
    if (set_ptr && copy_from_user(&uset, (const void *)set_ptr, sizeof(uset)) != 0)
        return (uint64_t)-14; /* -EFAULT */
    uint64_t newbits = uset << 1;

    if (oldset_ptr) {
        uint64_t old = t->sig_mask >> 1;
        if (copy_to_user((void *)oldset_ptr, &old, sizeof(old)) != 0) return (uint64_t)-14;
    }
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
        process_mark_exited(t->proc, 11 /* as if SIGSEGV */);
        t->state = THREAD_STATE_TERMINATED;
        serial_write_string("[signal] rt_sigreturn without frame\r\n");
        sched_schedule();
        while (1) __asm__ volatile("cli; hlt");
    }
    memcpy(regs, &f->r15, 16 * 8);
    t->sig_frame = NULL;
    /* rax is not part of SyscallRegs (it travels in the dispatcher's return
       register): the syscall the handler interrupted had its result saved
       in the frame at delivery -- returning it here puts it back in RAX.
       Restoring 0 instead (the old v1 contract) made e.g. waitpid() report
       0 whenever SIGCHLD's handler ran on its way back. */
    uint64_t rax = f->rax;
    memcpy(thread_fx_user(t), f->fx, FPU_AREA_MAX); /* restored by the syscall exit path */
    kfree(f);
    return rax;
}

void signal_raise_current(int sig) {
    if (sig <= 0 || sig >= 65) return;
    Thread *t = sched_current();
    t->sig_pending |= (1ULL << sig);
}

bool signal_raise_thread(uint64_t tid, uint64_t tgid, int sig) {
    if (sig <= 0 || sig >= 65 || tid == 0) return false;

    extern Thread *sched_get_thread_list(void);
    Thread *start = sched_get_thread_list();
    if (!start) return false;

    Thread *t = start;
    int guard = 0;
    do {
        if (t->id == tid && t->proc && t->proc->pid != 0) {
            if (tgid != 0 && (!t->proc || t->proc->pid != tgid)) {
                return false; /* real tgkill(2): ESRCH if tid isn't in thread group tgid */
            }
            t->sig_pending |= (1ULL << sig);
            return true;
        }
        t = t->next;
    } while (t != start && ++guard < 100000);

    return false; /* no such tid */
}
