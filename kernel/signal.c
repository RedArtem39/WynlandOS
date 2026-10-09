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
    uint64_t mask;    /* the blocked set to go back to (rt_sigreturn) */
    struct SignalFrame *prev;   /* the frame of a handler this one interrupted */
    uint64_t uc;                /* SA_SIGINFO: the ucontext on the user stack; rt_sigreturn
                                   takes the registers back from it (a handler may change
                                   them -- seccomp trap handlers set RAX, the result) */
    uint8_t fx[FPU_AREA_MAX]; /* the interrupted code's FPU state: the handler may
                        use those registers; rt_sigreturn puts it back */
} SignalFrame;

#define SA_SIGINFO_   0x00000004u
#define SA_RESTART_   0x10000000u
#define SA_NODEFER_   0x40000000u
#define SA_RESETHAND_ 0x80000000u
#define SIG_UNBLOCKABLE ((1ULL << 9) | (1ULL << 19))

/* signal numbers with default-ignore semantics (everything else with no
   handler terminates the process -- the classic Linux default set). The
   stop signals (STOP, TSTP, TTIN, TTOU) are ignored too: nothing is ever
   stopped here, and killing a shell's job for ^Z would be worse. */
static int default_ignored(int sig) {
    return sig == 13 /* PIPE */ || sig == 17 /* CHLD */ || sig == 23 /* URG */ ||
           sig == 18 /* CONT */ || sig == 28 /* WINCH */ ||
           sig == 19 || sig == 20 || sig == 21 || sig == 22;
}

/* Would `sig` do anything to t right now (run a handler, or kill)? What
   decides whether a sleeping thread is woken for it: an ignored or
   blocked signal must not cut a read() short with -EINTR. */
bool signal_wants_wake(Thread *t, int sig) {
    if (!t || !t->proc || sig <= 0 || sig >= 65) return false;
    if (sig == 9) return true;
    if (t->sig_mask & (1ULL << sig)) return false;
    uint64_t h = t->proc->sig_acts[sig].handler;
    if (h == 1) return false;
    if (h == 0 && default_ignored(sig)) return false;
    return true;
}

/* syscalls SA_RESTART restarts after the handler (Linux restarts these;
   poll/select/epoll/nanosleep/sigsuspend always end with -EINTR) */
static bool restartable(uint64_t nr) {
    switch (nr) {
        case 0: case 1: case 17: case 18: case 19: case 20:       /* read write pread pwrite readv writev */
        case 16: case 72: case 73:                                 /* ioctl fcntl flock */
        case 61: case 247:                                         /* wait4 waitid */
        case 43: case 288: case 42: case 44: case 45: case 46: case 47:  /* accept connect send/recv */
        case 2: case 257: case 202:                                /* open openat futex */
            return true;
    }
    return false;
}

/* rt_sigsuspend()'s temporary mask goes back once the signal it waited
   for was dealt with without a handler frame to carry it */
static void restore_suspend_mask(Thread *t) {
    if (t->sig_restore_mask) {
        t->sig_mask = t->sig_saved_mask;
        t->sig_restore_mask = false;
    }
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

/* execve(): handlers were addresses in the old image -- back to the
   default action (an ignored signal stays ignored, as on Linux); frames of
   handlers that were running are gone with it. The mask is kept. */
void signal_exec_reset(Thread *t, Process *p) {
    for (int sig = 1; sig < 65; sig++) {
        SigAct *sa = &p->sig_acts[sig];
        if (sa->handler != 1) memset(sa, 0, sizeof(*sa));
    }
    while (t->sig_frame) {
        SignalFrame *f = (SignalFrame *)t->sig_frame;
        t->sig_frame = f->prev;
        kfree(f);
    }
    t->sig_restore_mask = false;
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

    int sig;
    SigAct *act;
    for (;;) {
        uint64_t pending = t->sig_pending & ~t->sig_mask;
        if (!pending) { restore_suspend_mask(t); return 0; }
        sig = 1;
        while (sig < 65 && !(pending & (1ULL << sig))) sig++;
        if (sig >= 65) { t->sig_pending &= ~pending; restore_suspend_mask(t); return 0; }
        t->sig_pending &= ~(1ULL << sig);
        act = &t->proc->sig_acts[sig];
        if (act->handler == 1) continue;                            /* SIG_IGN */
        if (act->handler == 0 && default_ignored(sig)) continue;
        break;
    }

    if (act->handler == 0) {
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
    if (!kf) { restore_suspend_mask(t); return 0; } /* out of memory: drop the signal */
    memcpy(&kf->r15, regs, 16 * 8);
    kf->sig = sig;
    kf->rax = sysret;
    /* SA_RESTART: back to the syscall instruction with its number in RAX
       -- the interrupted call runs again after the handler */
    if ((int64_t)sysret == -4 /* EINTR */ && (act->flags & SA_RESTART_) && restartable(t->last_syscall)) {
        kf->rip -= 2;
        kf->rax = t->last_syscall;
    }
    memcpy(kf->fx, thread_fx_user(t), FPU_AREA_MAX);
    /* the mask the handler returns to: sigsuspend's caller's, not its
       temporary one */
    kf->mask = t->sig_restore_mask ? t->sig_saved_mask : t->sig_mask;
    t->sig_restore_mask = false;
    kf->prev = (SignalFrame *)t->sig_frame;
    t->sig_frame = (void *)kf;
    /* while it runs: sa_mask, and the signal itself unless SA_NODEFER */
    t->sig_mask |= (act->mask << 1) | ((act->flags & SA_NODEFER_) ? 0 : (1ULL << sig));
    t->sig_mask &= ~SIG_UNBLOCKABLE;
    uint64_t handler = act->handler, restorer = act->restorer, flags = act->flags;
    if (flags & SA_RESETHAND_) { act->handler = 0; act->flags &= ~SA_SIGINFO_; }

    /* Skip the 128-byte red zone below the interrupted RSP first (SysV
       x86-64 ABI: leaf code keeps live locals there without moving RSP).
       Writing the return address straight under RSP clobbered them --
       glibc saw it as "*** stack smashing detected ***" when a SIGCHLD
       handler ran on the way back from wait4(). */
    uint64_t sp = (regs->rsp - 128) & ~0xFULL;
    /* SA_SIGINFO: handler(sig, siginfo_t *, ucontext_t *), both on the
       user stack -- glibc-sized, zeroed, with what a handler may read:
       si_signo/si_code, uc_sigmask, and RIP/RSP in uc_mcontext */
    uint64_t uc = 0, si = 0;
    if (flags & SA_SIGINFO_) {
        sp -= 1024; uc = sp;
        sp -= 128;  si = sp;
    }
    sp -= 8; /* handler entry: RSP = 8 mod 16, as after a CALL */
    if (!user_prepare_write(sp, si ? (uc + 1024 - sp) : sizeof(uint64_t))) {
        /* no usable user stack for the handler: like Linux, the process
           dies of SIGSEGV instead of the kernel faulting on the store */
        t->sig_frame = kf->prev;
        kfree(kf);
        process_mark_exited(t->proc, 11);
        t->state = THREAD_STATE_TERMINATED;
        sched_schedule();
        while (1) __asm__ volatile("cli; hlt");
    }
    *(uint64_t *)(uintptr_t)sp = restorer; /* handler's RET target */
    kf->uc = uc;
    if (si) {
        memset((void *)(uintptr_t)si, 0, (size_t)(uc + 1024 - si));
        int32_t *sif = (int32_t *)(uintptr_t)si;
        sif[0] = sig;                                   /* si_signo; si_code 0 = SI_USER */
        if (sig == 31 && t->sys_trap) {                 /* SIGSYS from a seccomp filter */
            sif[1] = (int32_t)t->sys_trap_data;         /* si_errno: SECCOMP_RET_DATA */
            sif[2] = 1;                                 /* si_code: SYS_SECCOMP */
            *(uint64_t *)(uintptr_t)(si + 16) = t->sys_trap_ip;   /* si_call_addr */
            sif[6] = t->sys_trap_nr;                    /* si_syscall */
            sif[7] = (int32_t)0xC000003E;               /* si_arch: AUDIT_ARCH_X86_64 */
            t->sys_trap = false;
        }
        /* uc_mcontext.gregs[]: R8..R15, RDI, RSI, RBP, RBX, RDX, RAX,
           RCX, RSP, RIP, EFL (RCX/R11 as sysret leaves them) */
        uint64_t *g = (uint64_t *)(uintptr_t)uc + 5;
        g[0] = kf->r8;  g[1] = kf->r9;  g[2] = kf->r10; g[3] = kf->r11;
        g[4] = kf->r12; g[5] = kf->r13; g[6] = kf->r14; g[7] = kf->r15;
        g[8] = kf->rdi; g[9] = kf->rsi; g[10] = kf->rbp; g[11] = kf->rbx;
        g[12] = kf->rdx; g[13] = kf->rax; g[14] = kf->rip;
        g[15] = kf->rsp; g[16] = kf->rip; g[17] = kf->rflags;
        uint64_t *ucq = (uint64_t *)(uintptr_t)uc;
        ucq[37] = kf->mask >> 1;                        /* uc_sigmask (offset 296) */
    } else {
        t->sys_trap = false;
    }

    regs->rip = handler;
    regs->rsp = sp;
    regs->rdi = (uint64_t)(uint32_t)sig; /* handler(signum, ...) */
    regs->rsi = si;
    regs->rdx = uc;
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
    t->sig_frame = f->prev;
    t->sig_mask = f->mask & ~SIG_UNBLOCKABLE;
    if (f->uc) {
        /* what the handler left in its ucontext, as on Linux: the general
           registers, RIP/RSP (user addresses only), the signal mask */
        uint64_t g[18], umask;
        if (copy_from_user(g, (const void *)(uintptr_t)(f->uc + 40), sizeof(g)) != 0 ||
            copy_from_user(&umask, (const void *)(uintptr_t)(f->uc + 296), sizeof(umask)) != 0 ||
            g[16] >= SIG_USER_LIMIT || g[15] >= SIG_USER_LIMIT) {
            kfree(f);
            process_mark_exited(t->proc, 11);
            t->state = THREAD_STATE_TERMINATED;
            serial_write_string("[signal] rt_sigreturn: bad ucontext\r\n");
            sched_schedule();
            while (1) __asm__ volatile("cli; hlt");
        }
        regs->r8 = g[0];  regs->r9 = g[1];  regs->r10 = g[2]; regs->r11 = g[3];
        regs->r12 = g[4]; regs->r13 = g[5]; regs->r14 = g[6]; regs->r15 = g[7];
        regs->rdi = g[8]; regs->rsi = g[9]; regs->rbp = g[10]; regs->rbx = g[11];
        regs->rdx = g[12]; f->rax = g[13];
        regs->rsp = g[15]; regs->rip = g[16];
        t->sig_mask = (umask << 1) & ~SIG_UNBLOCKABLE;
    }
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
