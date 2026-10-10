/*
 * WynlandOS - sandboxing: seccomp-bpf, no_new_privs, capabilities,
 * chroot, and user, pid and network namespaces. The interface and what
 * differs from Linux: include/wynland/sandbox.h.
 *
 * Copyright (C) 2026 Red_Artem39. GPL-2.0-or-later.
 */
#include <wynland/sandbox.h>
#include <wynland/types.h>
#include <wynland/heap.h>
#include <wynland/process.h>
#include <wynland/sched.h>
#include <wynland/signal.h>
#include <wynland/usercopy.h>
#include <wynland/vfs.h>

extern void serial_write_string(const char *str);

#define EPERM   1
#define ESRCH   3
#define EFAULT  14
#define EINVAL  22
#define ENOMEM  12
#define EACCES  13
#define ENOSYS  38
#define EOPNOTSUPP 95
#define EUSERS  87

static Process *cur(void) { Thread *t = sched_current(); return t ? t->proc : NULL; }

/* mount namespaces (further down) */
static MntNs *mntns_of(const Process *p);
static MntNs *mntns_copy(MntNs *from, UserNs *owner);
static void mntns_unref(MntNs *ns);

static uint64_t caps_of(const Process *p) {
    if (!p) return 0;
    if (p->caps_set) return p->cap_eff;
    return (p->uid == 0 && !p->userns) ? CAP_ALL : 0;
}
static bool has_cap(const Process *p, int cap) { return (caps_of(p) >> cap) & 1; }

/* =====================================================================
 * seccomp
 * ===================================================================== */

typedef struct { uint16_t code; uint8_t jt, jf; uint32_t k; } SockFilter;

struct SeccompFilter {
    uint32_t refs;
    struct SeccompFilter *prev;
    uint16_t len;
    bool log;
    SockFilter insns[];
};

typedef struct {
    int32_t  nr;
    uint32_t arch;
    uint64_t ip;
    uint64_t args[6];
} SeccompData;
_Static_assert(sizeof(SeccompData) == 64, "struct seccomp_data");

#define AUDIT_ARCH_X86_64 0xC000003Eu

#define RET_KILL_PROCESS 0x80000000u
#define RET_KILL_THREAD  0x00000000u
#define RET_TRAP         0x00030000u
#define RET_ERRNO        0x00050000u
#define RET_USER_NOTIF   0x7fc00000u
#define RET_TRACE        0x7ff00000u
#define RET_LOG          0x7ffc0000u
#define RET_ALLOW        0x7fff0000u
#define RET_ACTION_FULL  0xffff0000u
#define RET_DATA         0x0000ffffu

#define BPF_MAXINSNS     4096
#define MAX_INSNS_PATH   32768
#define BPF_MEMWORDS     16

/* classic BPF */
#define BPF_CLASS(c) ((c) & 0x07)
#define BPF_LD   0x00
#define BPF_LDX  0x01
#define BPF_ST   0x02
#define BPF_STX  0x03
#define BPF_ALU  0x04
#define BPF_JMP  0x05
#define BPF_RET  0x06
#define BPF_MISC 0x07
#define BPF_SIZE(c) ((c) & 0x18)
#define BPF_W    0x00
#define BPF_MODE(c) ((c) & 0xe0)
#define BPF_IMM  0x00
#define BPF_ABS  0x20
#define BPF_MEM  0x60
#define BPF_LEN  0x80
#define BPF_OP(c) ((c) & 0xf0)
#define BPF_ADD 0x00
#define BPF_SUB 0x10
#define BPF_MUL 0x20
#define BPF_DIV 0x30
#define BPF_OR  0x40
#define BPF_AND 0x50
#define BPF_LSH 0x60
#define BPF_RSH 0x70
#define BPF_NEG 0x80
#define BPF_MOD 0x90
#define BPF_XOR 0xa0
#define BPF_JA   0x00
#define BPF_JEQ  0x10
#define BPF_JGT  0x20
#define BPF_JGE  0x30
#define BPF_JSET 0x40
#define BPF_SRC(c) ((c) & 0x08)
#define BPF_K 0x00
#define BPF_X 0x08
#define BPF_RVAL(c) ((c) & 0x18)
#define BPF_A 0x10
#define BPF_MISCOP(c) ((c) & 0xf8)
#define BPF_TAX 0x00
#define BPF_TXA 0x80

/* What seccomp accepts (Linux's seccomp_check_filter + bpf_check_classic):
   word loads from seccomp_data (aligned, inside it), the data's length,
   immediates and the 16 scratch words, ALU, forward jumps that stay in
   the program, RET; the last instruction a RET. A load of the length is
   turned into an immediate 64. */
static bool bpf_check(SockFilter *f, uint32_t len) {
    if (len == 0 || len > BPF_MAXINSNS) return false;
    for (uint32_t pc = 0; pc < len; pc++) {
        SockFilter *i = &f[pc];
        uint16_t c = i->code;
        switch (BPF_CLASS(c)) {
        case BPF_LD:
        case BPF_LDX:
            if (BPF_SIZE(c) != BPF_W) return false;
            switch (BPF_MODE(c)) {
            case BPF_ABS:
                if (BPF_CLASS(c) != BPF_LD || i->k >= sizeof(SeccompData) || (i->k & 3)) return false;
                break;
            case BPF_LEN:
                i->code = (uint16_t)(BPF_CLASS(c) | BPF_W | BPF_IMM);
                i->k = sizeof(SeccompData);
                break;
            case BPF_IMM:
                break;
            case BPF_MEM:
                if (i->k >= BPF_MEMWORDS) return false;
                break;
            default:
                return false;
            }
            if (c != i->code) break;
            if (c & ~0xffu) return false;
            break;
        case BPF_ST:
        case BPF_STX:
            if (c != (uint16_t)BPF_CLASS(c) || i->k >= BPF_MEMWORDS) return false;
            break;
        case BPF_ALU:
            switch (BPF_OP(c)) {
            case BPF_ADD: case BPF_SUB: case BPF_MUL: case BPF_OR: case BPF_AND: case BPF_XOR:
                break;
            case BPF_DIV: case BPF_MOD:
                if (BPF_SRC(c) == BPF_K && i->k == 0) return false;
                break;
            case BPF_LSH: case BPF_RSH:
                if (BPF_SRC(c) == BPF_K && i->k >= 32) return false;
                break;
            case BPF_NEG:
                if (BPF_SRC(c) != BPF_K) return false;
                break;
            default:
                return false;
            }
            if (c & ~0xffu) return false;
            break;
        case BPF_JMP:
            if (BPF_OP(c) == BPF_JA) {
                if (BPF_SRC(c) != BPF_K || (uint64_t)pc + 1 + i->k >= len) return false;
            } else {
                switch (BPF_OP(c)) {
                case BPF_JEQ: case BPF_JGT: case BPF_JGE: case BPF_JSET: break;
                default: return false;
                }
                if (pc + 1u + i->jt >= len || pc + 1u + i->jf >= len) return false;
            }
            if (c & ~0xffu) return false;
            break;
        case BPF_RET:
            if (BPF_RVAL(c) != BPF_K && BPF_RVAL(c) != BPF_A) return false;
            if (c & ~0xffu || (c & 0xe0)) return false;
            break;
        case BPF_MISC:
            if (BPF_MISCOP(c) != BPF_TAX && BPF_MISCOP(c) != BPF_TXA) return false;
            if (c & ~0xffu || (c & 0x07) != BPF_MISC || (c & 0x78)) return false;
            break;
        default:
            return false;
        }
    }
    return BPF_CLASS(f[len - 1].code) == BPF_RET;
}

static uint32_t bpf_run(const struct SeccompFilter *flt, const SeccompData *d) {
    const uint8_t *data = (const uint8_t *)d;
    uint32_t A = 0, X = 0, M[BPF_MEMWORDS];
    memset(M, 0, sizeof(M));
    for (uint32_t pc = 0; pc < flt->len; pc++) {
        const SockFilter *i = &flt->insns[pc];
        uint16_t c = i->code;
        uint32_t v = BPF_SRC(c) == BPF_X ? X : i->k;
        switch (BPF_CLASS(c)) {
        case BPF_LD:
            if (BPF_MODE(c) == BPF_ABS) memcpy(&A, data + i->k, 4);
            else if (BPF_MODE(c) == BPF_IMM) A = i->k;
            else A = M[i->k];                   /* BPF_MEM */
            break;
        case BPF_LDX:
            X = BPF_MODE(c) == BPF_IMM ? i->k : M[i->k];
            break;
        case BPF_ST:  M[i->k] = A; break;
        case BPF_STX: M[i->k] = X; break;
        case BPF_ALU:
            switch (BPF_OP(c)) {
            case BPF_ADD: A += v; break;
            case BPF_SUB: A -= v; break;
            case BPF_MUL: A *= v; break;
            case BPF_DIV: if (!v) return 0; A /= v; break;   /* by zero X: the program ends, returning 0 (kill) */
            case BPF_MOD: if (!v) return 0; A %= v; break;
            case BPF_OR:  A |= v; break;
            case BPF_AND: A &= v; break;
            case BPF_XOR: A ^= v; break;
            case BPF_LSH: A = v < 32 ? A << v : 0; break;
            case BPF_RSH: A = v < 32 ? A >> v : 0; break;
            case BPF_NEG: A = (uint32_t)-(int32_t)A; break;
            }
            break;
        case BPF_JMP:
            switch (BPF_OP(c)) {
            case BPF_JA:   pc += i->k; break;
            case BPF_JEQ:  pc += (A == v) ? i->jt : i->jf; break;
            case BPF_JGT:  pc += (A > v) ? i->jt : i->jf; break;
            case BPF_JGE:  pc += (A >= v) ? i->jt : i->jf; break;
            case BPF_JSET: pc += (A & v) ? i->jt : i->jf; break;
            }
            break;
        case BPF_RET:
            return BPF_RVAL(c) == BPF_A ? A : i->k;
        case BPF_MISC:
            if (BPF_MISCOP(c) == BPF_TAX) X = A; else A = X;
            break;
        }
    }
    return RET_KILL_PROCESS;   /* a checked program never gets here */
}

static void filter_ref(struct SeccompFilter *f) { if (f) f->refs++; }
static void filter_unref(struct SeccompFilter *f) {
    while (f && --f->refs == 0) {
        struct SeccompFilter *prev = f->prev;
        kfree(f);
        f = prev;
    }
}

/* strict mode kills with SIGKILL, a filter's KILL action with SIGSYS */
static void seccomp_kill(Process *p, uint64_t nr, int sig) {
    char b[24];
    extern void uint_to_str(uint64_t val, char *buf);
    serial_write_string("[seccomp] killed ");
    serial_write_string(p->exe_path);
    serial_write_string(": syscall ");
    uint_to_str(nr, b);
    serial_write_string(b);
    serial_write_string("\r\n");
    signal_kill_process(p, sig);
    sched_schedule();
    for (;;) __asm__ volatile("cli; hlt");
}

bool seccomp_filter_syscall(uint64_t nr, const uint64_t a[6], uint64_t ip, uint64_t *ret) {
    Process *p = cur();
    if (!p || !p->seccomp_mode) return false;
    if (p->seccomp_mode == 1) {
        /* strict: read, write, exit, rt_sigreturn -- the rest kill */
        if (nr == 0 || nr == 1 || nr == 60 || nr == 15) return false;
        seccomp_kill(p, nr, 9);
    }
    SeccompData d;
    d.nr = (int32_t)nr;
    d.arch = AUDIT_ARCH_X86_64;
    d.ip = ip;
    for (int k = 0; k < 6; k++) d.args[k] = a[k];
    /* every filter runs; the most restrictive action wins (the smallest
       action value as a signed number: KILL_PROCESS, KILL_THREAD, TRAP,
       ERRNO, USER_NOTIF, TRACE, LOG, ALLOW) */
    uint32_t r = RET_ALLOW;
    bool log = false;
    for (const struct SeccompFilter *f = p->seccomp; f; f = f->prev) {
        uint32_t cr = bpf_run(f, &d);
        if ((int32_t)(cr & RET_ACTION_FULL) < (int32_t)(r & RET_ACTION_FULL)) { r = cr; log = f->log; }
    }
    uint32_t action = r & RET_ACTION_FULL, data = r & RET_DATA;
    switch (action) {
    case RET_ALLOW:
        return false;
    case RET_LOG:
        serial_write_string(log ? "[seccomp] logged (FLAG_LOG) syscall in " : "[seccomp] logged syscall in ");
        serial_write_string(p->exe_path);
        serial_write_string("\r\n");
        return false;
    case RET_ERRNO:
        *ret = (uint64_t)-(int64_t)(data > 4095 ? 4095 : data);
        return true;
    case RET_TRAP: {
        /* SIGSYS now, unblockable for it; the handler sees the call in
           its siginfo (si_syscall, si_call_addr, si_arch, si_errno = data)
           and the syscall's registers in its ucontext, RAX holding the
           syscall number -- what it leaves there is the result */
        Thread *t = sched_current();
        t->sys_trap = true;
        t->sys_trap_nr = (int32_t)nr;
        t->sys_trap_data = data;
        t->sys_trap_ip = ip;
        t->sig_mask &= ~(1ULL << 31);
        if (p->sig_acts[31].handler == 1) p->sig_acts[31].handler = 0;   /* ignored: back to fatal */
        t->sig_pending |= 1ULL << 31;
        *ret = nr;
        return true;
    }
    case RET_TRACE:
    case RET_USER_NOTIF:
        *ret = (uint64_t)-ENOSYS;   /* no tracer, no listener */
        return true;
    default:                        /* KILL_THREAD, KILL_PROCESS, anything else */
        seccomp_kill(p, nr, 31);
    }
    return true;
}

#define SECCOMP_SET_MODE_STRICT   0
#define SECCOMP_SET_MODE_FILTER   1
#define SECCOMP_GET_ACTION_AVAIL  2
#define SECCOMP_GET_NOTIF_SIZES   3
#define FLAG_TSYNC        1u
#define FLAG_LOG          2u
#define FLAG_SPEC_ALLOW   4u
#define FLAG_NEW_LISTENER 8u
#define FLAG_TSYNC_ESRCH  16u

int64_t sandbox_seccomp(uint32_t op, uint32_t flags, uint64_t uargs) {
    Process *p = cur();
    switch (op) {
    case SECCOMP_SET_MODE_STRICT:
        if (flags || uargs) return -EINVAL;
        if (p->seccomp_mode == 2) return -EINVAL;
        p->seccomp_mode = 1;
        return 0;
    case SECCOMP_SET_MODE_FILTER: {
        if (flags & ~(FLAG_TSYNC | FLAG_LOG | FLAG_SPEC_ALLOW | FLAG_TSYNC_ESRCH)) return -EINVAL;
        if (p->seccomp_mode == 1) return -EINVAL;
        /* a filter may only be installed by a process that cannot gain
           privileges any more, or by one that has them anyway */
        if (!p->no_new_privs && !has_cap(p, CAP_SYS_ADMIN)) return -EACCES;
        struct { uint16_t len; uint16_t pad[3]; uint64_t filter; } prog;
        if (!uargs || copy_from_user(&prog, (const void *)uargs, sizeof(prog)) != 0) return -EFAULT;
        if (prog.len == 0 || prog.len > BPF_MAXINSNS) return -EINVAL;
        uint32_t path = prog.len + 4;
        for (struct SeccompFilter *f = p->seccomp; f; f = f->prev) path += f->len + 4;
        if (path > MAX_INSNS_PATH) return -ENOMEM;
        struct SeccompFilter *nf = (struct SeccompFilter *)kmalloc(sizeof(*nf) + prog.len * sizeof(SockFilter));
        if (!nf) return -ENOMEM;
        if (copy_from_user(nf->insns, (const void *)prog.filter, prog.len * sizeof(SockFilter)) != 0) {
            kfree(nf);
            return -EFAULT;
        }
        nf->len = prog.len;
        if (!bpf_check(nf->insns, nf->len)) { kfree(nf); return -EINVAL; }
        nf->refs = 1;
        nf->log = (flags & FLAG_LOG) != 0;
        nf->prev = p->seccomp;           /* the old chain's reference moves into nf */
        p->seccomp = nf;
        p->seccomp_mode = 2;
        return 0;
    }
    case SECCOMP_GET_ACTION_AVAIL: {
        uint32_t act;
        if (!uargs || copy_from_user(&act, (const void *)uargs, 4) != 0) return -EFAULT;
        switch (act) {
        case RET_KILL_PROCESS: case RET_KILL_THREAD: case RET_TRAP: case RET_ERRNO:
        case RET_TRACE: case RET_LOG: case RET_ALLOW:
            return 0;
        }
        return -EOPNOTSUPP;
    }
    default:
        return -EINVAL;
    }
}

/* =====================================================================
 * prctl, capabilities
 * ===================================================================== */

#define PR_SET_PDEATHSIG     1
#define PR_GET_PDEATHSIG     2
#define PR_GET_DUMPABLE      3
#define PR_SET_DUMPABLE      4
#define PR_SET_NAME         15
#define PR_GET_NAME         16
#define PR_GET_SECCOMP      21
#define PR_SET_SECCOMP      22
#define PR_CAPBSET_READ     23
#define PR_CAPBSET_DROP     24
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39

int64_t sandbox_prctl(int option, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    Process *p = cur();
    Thread *t = sched_current();
    switch (option) {
    case PR_SET_NO_NEW_PRIVS:
        if (a2 != 1 || a3 || a4 || a5) return -EINVAL;
        p->no_new_privs = true;
        return 0;
    case PR_GET_NO_NEW_PRIVS:
        if (a2 || a3 || a4 || a5) return -EINVAL;
        return p->no_new_privs ? 1 : 0;
    case PR_GET_SECCOMP:
        return p->seccomp_mode;
    case PR_SET_SECCOMP:
        if (a2 == 1) return sandbox_seccomp(SECCOMP_SET_MODE_STRICT, 0, 0);
        if (a2 == 2) return sandbox_seccomp(SECCOMP_SET_MODE_FILTER, 0, a3);
        return -EINVAL;
    case PR_SET_NAME: {
        char n[16];
        memset(n, 0, sizeof(n));
        for (int k = 0; k < 15; k++) {
            if (copy_from_user(&n[k], (const void *)(a2 + (uint64_t)k), 1) != 0) return -EFAULT;
            if (!n[k]) break;
        }
        n[15] = 0;
        memcpy(t->comm, n, 16);
        return 0;
    }
    case PR_GET_NAME: {
        char n[16];
        memset(n, 0, sizeof(n));
        if (t->comm[0]) {
            memcpy(n, t->comm, 16);
        } else {                                   /* the program's file name */
            const char *e = p->exe_path, *b = e;
            for (; *e; e++) if (*e == '/') b = e + 1;
            for (int k = 0; k < 15 && b[k]; k++) n[k] = b[k];
        }
        return copy_to_user((void *)a2, n, 16) != 0 ? -EFAULT : 0;
    }
    case PR_SET_DUMPABLE:
        if (a2 > 1) return -EINVAL;
        p->nondumpable = a2 == 0;
        return 0;
    case PR_GET_DUMPABLE:
        return p->nondumpable ? 0 : 1;
    case PR_SET_PDEATHSIG:
        if (a2 > 64) return -EINVAL;
        p->pdeathsig = (int)a2;
        return 0;
    case PR_GET_PDEATHSIG:
        return copy_to_user((void *)a2, &p->pdeathsig, 4) != 0 ? -EFAULT : 0;
    case PR_CAPBSET_READ:
        return a2 <= 40 ? 1 : -EINVAL;
    case PR_CAPBSET_DROP:
        if (a2 > 40) return -EINVAL;
        return has_cap(p, 8 /* CAP_SETPCAP */) ? 0 : -EPERM;
    default:
        return 0;    /* the others: accepted, as before */
    }
}

/* capget/capset: _LINUX_CAPABILITY_VERSION_3, two 32-bit words per set */
#define CAPV3 0x20080522u

int64_t sandbox_capget(uint64_t hdr, uint64_t data) {
    struct { uint32_t version; int32_t pid; } h;
    if (!hdr || copy_from_user(&h, (const void *)hdr, sizeof(h)) != 0) return -EFAULT;
    if (h.version != CAPV3 && h.version != 0x19980330u && h.version != 0x20071026u) {
        h.version = CAPV3;
        copy_to_user((void *)hdr, &h, sizeof(h));
        return data ? -EINVAL : 0;
    }
    Process *p = cur();
    if (h.pid > 0) {
        Process *q = process_find_by_pid(pid_from_ns((uint64_t)h.pid, p));
        if (!q || q->reaped) return -ESRCH;
        p = q;
    }
    if (!data) return 0;
    uint64_t e = caps_of(p), pr = p->caps_set ? p->cap_prm : e, in = p->caps_set ? p->cap_inh : 0;
    uint32_t d[6] = { (uint32_t)e, (uint32_t)pr, (uint32_t)in,
                      (uint32_t)(e >> 32), (uint32_t)(pr >> 32), (uint32_t)(in >> 32) };
    uint32_t words = h.version == 0x19980330u ? 3 : 6;
    return copy_to_user((void *)data, d, words * 4) != 0 ? -EFAULT : 0;
}

int64_t sandbox_capset(uint64_t hdr, uint64_t data) {
    struct { uint32_t version; int32_t pid; } h;
    if (!hdr || copy_from_user(&h, (const void *)hdr, sizeof(h)) != 0) return -EFAULT;
    if (h.version != CAPV3 && h.version != 0x20071026u) return -EINVAL;
    Process *p = cur();
    if (h.pid != 0 && pid_from_ns((uint64_t)h.pid, p) != p->pid) return -EPERM;
    uint32_t d[6];
    if (!data || copy_from_user(d, (const void *)data, sizeof(d)) != 0) return -EFAULT;
    uint64_t e = d[0] | ((uint64_t)d[3] << 32), pr = d[1] | ((uint64_t)d[4] << 32),
             in = d[2] | ((uint64_t)d[5] << 32);
    uint64_t old_prm = p->caps_set ? p->cap_prm : caps_of(p);
    /* only ever less: permitted within the old permitted, effective within
       the new permitted */
    if ((pr & ~old_prm) || (e & ~pr)) return -EPERM;
    p->caps_set = true;
    p->cap_eff = e & CAP_ALL;
    p->cap_prm = pr & CAP_ALL;
    p->cap_inh = in & CAP_ALL;
    return 0;
}

/* =====================================================================
 * user namespaces
 * ===================================================================== */

struct UserNs {
    struct UserNs *parent;
    uint32_t level;
    uint32_t owner;                 /* the creator's euid, global */
    bool uid_set, gid_set;
    uint32_t uid_in, uid_out, uid_cnt;     /* out: ids of the parent namespace */
    uint32_t gid_in, gid_out, gid_cnt;
    bool setgroups_deny;
    uint32_t id;
};
#define NS_MAX_LEVEL 8
static uint32_t g_ns_ids = 1;

#define NOID 0xFFFFFFFFu

/* an id of ns (inner) as a global id */
static uint32_t id_up(const UserNs *ns, uint32_t v, bool gid) {
    for (; ns; ns = ns->parent) {
        bool set = gid ? ns->gid_set : ns->uid_set;
        uint32_t in = gid ? ns->gid_in : ns->uid_in, out = gid ? ns->gid_out : ns->uid_out,
                 cnt = gid ? ns->gid_cnt : ns->uid_cnt;
        if (!set || v < in || v - in >= cnt) return NOID;
        v = v - in + out;
    }
    return v;
}

/* a global id as seen inside ns */
static uint32_t id_down(const UserNs *ns, uint32_t v, bool gid) {
    if (!ns) return v;
    v = id_down(ns->parent, v, gid);
    if (v == NOID) return NOID;
    bool set = gid ? ns->gid_set : ns->uid_set;
    uint32_t in = gid ? ns->gid_in : ns->uid_in, out = gid ? ns->gid_out : ns->uid_out,
             cnt = gid ? ns->gid_cnt : ns->uid_cnt;
    if (!set || v < out || v - out >= cnt) return NOID;
    return v - out + in;
}

uint32_t uid_to_ns(const Process *p, uint32_t g) { uint32_t v = id_down(p ? p->userns : NULL, g, false); return v == NOID ? 65534 : v; }
uint32_t gid_to_ns(const Process *p, uint32_t g) { uint32_t v = id_down(p ? p->userns : NULL, g, true); return v == NOID ? 65534 : v; }
uint32_t uid_from_ns(const Process *p, uint32_t i) { return i == NOID ? NOID : id_up(p ? p->userns : NULL, i, false); }
uint32_t gid_from_ns(const Process *p, uint32_t i) { return i == NOID ? NOID : id_up(p ? p->userns : NULL, i, true); }
bool sandbox_setgroups_denied(const Process *p) {
    for (const UserNs *ns = p ? p->userns : NULL; ns; ns = ns->parent) if (ns->setgroups_deny) return true;
    return false;
}

static UserNs *userns_new(Process *p) {
    uint32_t level = p->userns ? p->userns->level + 1 : 1;
    if (level > NS_MAX_LEVEL) return NULL;
    UserNs *ns = (UserNs *)kmalloc(sizeof(UserNs));
    if (!ns) return NULL;
    memset(ns, 0, sizeof(*ns));
    ns->parent = p->userns;
    ns->level = level;
    ns->owner = p->uid;
    ns->id = g_ns_ids++;
    /* "deny" in setgroups is inherited, as on Linux (and cannot be undone) */
    ns->setgroups_deny = p->userns && p->userns->setgroups_deny;
    return ns;
}

/* =====================================================================
 * pid namespaces: a table per namespace of (global id, its id there)
 * ===================================================================== */

struct PidNs {
    struct PidNs *parent;
    uint32_t level;
    uint32_t id;
    uint64_t next;                  /* the next id handed out in it */
    uint32_t n, cap;
    struct { uint64_t g, l; } *map;
    uint64_t init;                  /* global pid of its init (its id 1) */
    bool dead;                      /* init is gone: no new processes */
};

static PidNs *pidns_new(PidNs *parent) {
    uint32_t level = parent ? parent->level + 1 : 1;
    if (level > NS_MAX_LEVEL) return NULL;
    PidNs *ns = (PidNs *)kmalloc(sizeof(PidNs));
    if (!ns) return NULL;
    memset(ns, 0, sizeof(*ns));
    ns->parent = parent;
    ns->level = level;
    ns->next = 1;
    ns->id = g_ns_ids++;
    return ns;
}

static bool pidns_add(PidNs *ns, uint64_t g, uint64_t l) {
    if (ns->n == ns->cap) {
        uint32_t nc = ns->cap ? ns->cap * 2 : 16;
        void *nm = kmalloc(nc * sizeof(ns->map[0]));
        if (!nm) return false;
        if (ns->map) { memcpy(nm, ns->map, ns->n * sizeof(ns->map[0])); kfree(ns->map); }
        ns->map = nm;
        ns->cap = nc;
    }
    ns->map[ns->n].g = g;
    ns->map[ns->n].l = l;
    ns->n++;
    return true;
}

/* room for one more id in every table of ns's chain (so registering the
   new process or thread cannot fail half way) */
static bool pidns_reserve(PidNs *ns) {
    for (; ns; ns = ns->parent) {
        if (ns->n < ns->cap) continue;
        uint32_t nc = ns->cap ? ns->cap * 2 : 16;
        void *nm = kmalloc(nc * sizeof(ns->map[0]));
        if (!nm) return false;
        if (ns->map) { memcpy(nm, ns->map, ns->n * sizeof(ns->map[0])); kfree(ns->map); }
        ns->map = nm;
        ns->cap = nc;
    }
    return true;
}

void pidns_register(uint64_t global, Process *p) {
    for (PidNs *ns = p ? p->pidns : NULL; ns; ns = ns->parent) {
        uint64_t l = ns->next++;
        if (l == 1 && !ns->init) ns->init = global;
        pidns_add(ns, global, l);
    }
}

uint64_t pid_to_ns(uint64_t g, const Process *viewer) {
    const PidNs *ns = viewer ? viewer->pidns : NULL;
    if (!ns || g == 0) return g;
    for (uint32_t k = 0; k < ns->n; k++) if (ns->map[k].g == g) return ns->map[k].l;
    return 0;
}

uint64_t pid_from_ns(uint64_t l, const Process *viewer) {
    const PidNs *ns = viewer ? viewer->pidns : NULL;
    if (!ns || l == 0) return l;
    for (uint32_t k = 0; k < ns->n; k++) if (ns->map[k].l == l) return ns->map[k].g;
    return 0;
}

/* =====================================================================
 * namespaces: unshare/clone
 * ===================================================================== */

#define CLONE_NEWTIME   0x00000080ull
#define CLONE_FS        0x00000200ull
#define CLONE_FILES     0x00000400ull
#define CLONE_SIGHAND   0x00000800ull
#define CLONE_VM        0x00000100ull
#define CLONE_THREAD    0x00010000ull
#define CLONE_NEWNS     0x00020000ull
#define CLONE_SYSVSEM   0x00040000ull
#define CLONE_NEWCGROUP 0x02000000ull
#define CLONE_NEWUTS    0x04000000ull
#define CLONE_NEWIPC    0x08000000ull
#define CLONE_NEWUSER   0x10000000ull
#define CLONE_NEWPID    0x20000000ull
#define CLONE_NEWNET    0x40000000ull
#define NS_FLAGS (CLONE_NEWTIME | CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | \
                  CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET)

static uint32_t g_netns_ids = 1;

bool sandbox_ns_admin(const Process *p) { return has_cap(p, CAP_SYS_ADMIN); }
uint32_t sandbox_netns(const Process *p) { return p ? p->netns : 0; }

/* may p make the namespaces in `flags`? (a new user namespace first gives
   it the right to the others, as on Linux) */
static int64_t ns_check(const Process *p, uint64_t flags) {
    if (flags & CLONE_NEWUSER) {
        if (p->chrooted) return -EPERM;                  /* not from inside a chroot */
        if ((p->userns ? p->userns->level : 0) >= NS_MAX_LEVEL) return -EUSERS;
        return 0;
    }
    if ((flags & (NS_FLAGS & ~CLONE_NEWUSER)) && !has_cap(p, CAP_SYS_ADMIN)) return -EPERM;
    if ((flags & CLONE_NEWPID) && (p->pidns_child ? p->pidns_child->level : (p->pidns ? p->pidns->level : 0)) >= NS_MAX_LEVEL)
        return -EUSERS;
    return 0;
}

int64_t sandbox_unshare(uint64_t flags) {
    Process *p = cur();
    if (flags & ~(NS_FLAGS | CLONE_FS | CLONE_FILES | CLONE_SYSVSEM | CLONE_THREAD | CLONE_VM | CLONE_SIGHAND))
        return -EINVAL;
    if ((flags & (CLONE_THREAD | CLONE_VM | CLONE_SIGHAND | CLONE_NEWUSER)) && p->thread_count > 1)
        return -EINVAL;
    int64_t r = ns_check(p, flags);
    if (r) return r;
    UserNs *un = NULL;
    PidNs *pn = NULL;
    if (flags & CLONE_NEWUSER) {
        un = userns_new(p);
        if (!un) return -ENOMEM;
    }
    if (flags & CLONE_NEWPID) {
        pn = pidns_new(p->pidns_child ? p->pidns_child : p->pidns);
        if (!pn) { if (un) kfree(un); return -ENOMEM; }
    }
    MntNs *mn = NULL;
    if (flags & CLONE_NEWNS) {
        mn = mntns_copy(mntns_of(p), un ? un : p->userns);
        if (!mn) { if (un) kfree(un); if (pn) kfree(pn); return -ENOMEM; }
    }
    if (mn) { mntns_unref(p->mntns); p->mntns = mn; }
    if (un) {                    /* the new user namespace's creator has every capability in it */
        p->userns = un;
        p->caps_set = true;
        p->cap_eff = p->cap_prm = CAP_ALL;
        p->cap_inh = 0;
    }
    if (pn) p->pidns_child = pn;
    if (flags & CLONE_NEWNET) p->netns = g_netns_ids++;
    return 0;
}

/* the namespaces the fork in progress puts its child in, made before the
   fork so that it either gets them or fails */
static UserNs *g_pend_user;
static PidNs  *g_pend_pid;
static MntNs  *g_pend_mnt;

int64_t sandbox_clone_check(uint64_t flags) {
    g_pend_user = NULL;
    g_pend_pid = NULL;
    g_pend_mnt = NULL;
    if (!(flags & NS_FLAGS)) return 0;
    Process *p = cur();
    if (flags & CLONE_NEWUSER) {
        if (p->chrooted) return -EPERM;
        if ((p->userns ? p->userns->level : 0) >= NS_MAX_LEVEL) return -EUSERS;
        /* the child is admin in its new namespace: the rest is allowed */
    } else {
        int64_t r = ns_check(p, flags);
        if (r) return r;
    }
    if (flags & CLONE_NEWUSER) {
        g_pend_user = userns_new(p);
        if (!g_pend_user) return -ENOMEM;
    }
    if (flags & CLONE_NEWPID) {
        g_pend_pid = pidns_new(p->pidns_child ? p->pidns_child : p->pidns);
        if (!g_pend_pid) {
            if (g_pend_user) kfree(g_pend_user);
            g_pend_user = NULL;
            return -ENOMEM;
        }
    }
    if (flags & CLONE_NEWNS) {
        g_pend_mnt = mntns_copy(mntns_of(p), g_pend_user ? g_pend_user : p->userns);
        if (!g_pend_mnt) { sandbox_clone_abort(); return -ENOMEM; }
    }
    return 0;
}

/* the fork failed: what was made for it goes */
void sandbox_clone_abort(void) {
    if (g_pend_user) kfree(g_pend_user);
    if (g_pend_pid) kfree(g_pend_pid);
    mntns_unref(g_pend_mnt);
    g_pend_user = NULL;
    g_pend_pid = NULL;
    g_pend_mnt = NULL;
}

void sandbox_clone_apply(Process *child, uint64_t flags) {
    if (g_pend_user) {
        child->userns = g_pend_user;
        child->caps_set = true;
        child->cap_eff = child->cap_prm = CAP_ALL;
        child->cap_inh = 0;
        g_pend_user = NULL;
    }
    if (flags & CLONE_NEWNET) child->netns = g_netns_ids++;
    if (g_pend_mnt) {
        mntns_unref(child->mntns);
        child->mntns = g_pend_mnt;
        g_pend_mnt = NULL;
    }
}

/* May a new process be made into p's children's pid namespace (its init
   alive, room in the tables)? Before fork/spawn create anything. */
int64_t sandbox_fork_check(Process *p) {
    PidNs *ns = g_pend_pid ? g_pend_pid : (p->pidns_child ? p->pidns_child : p->pidns);
    for (PidNs *n = ns; n; n = n->parent) if (n->dead) return -ENOMEM;
    return pidns_reserve(ns) ? 0 : -ENOMEM;
}

/* the same for a new thread of p (it lives in p's own pid namespace) */
int64_t sandbox_thread_check(Process *p) {
    for (PidNs *n = p->pidns; n; n = n->parent) if (n->dead) return -ENOMEM;
    return pidns_reserve(p->pidns) ? 0 : -ENOMEM;
}


/* =====================================================================
 * mount namespaces
 *
 * Every mount is of the one ext2 filesystem: a bind mount shows a
 * directory (or file) at another one's place; a tmpfs is a fresh directory
 * under /.tmpfs (root-only), given to whoever mounted it, removed by the
 * flusher once nothing mounts it. The path walker (drivers/fs/ext2.c)
 * steps from a mountpoint to the mount's root (mnt_enter) and, for ".." at
 * a mount's root, to the mountpoint's directory (mnt_leave) -- never to
 * the real parent of what is mounted.
 * ===================================================================== */

typedef struct Tmpfs {
    uint32_t refs;
    char path[40];
} Tmpfs;

typedef struct Mount {
    uint32_t mp;                /* the inode it covers, as reached from where it was mounted */
    uint32_t mp_parent;         /* the directory holding the mountpoint */
    uint32_t root;              /* what it shows */
    struct Mount *in;           /* the mount the mountpoint lies in (NULL: none) */
    bool ro;
    bool locked;                /* inherited by a less privileged namespace: stays, stays read-only */
    Tmpfs *tmp;                 /* the tmpfs it shows (or a part of): kept while it is mounted */
    struct Mount *next;         /* newest first */
} Mount;

struct MntNs {
    uint32_t refs;
    uint32_t id;
    Mount *mounts;
    UserNs *owner;              /* whose capabilities may change it */
    uint32_t count;
};

#define MNT_MAX 256             /* mounts per namespace */

static MntNs g_init_mnt = { 1u << 30, 0, NULL, NULL, 0 };

/* the inodes a mount shows and covers stay (unlinked, they are orphans
   until it goes): a deleted mount root's inode could be reused */
extern bool ext2_pin_inode(uint32_t inum);
extern void ext2_unpin_inode(uint32_t inum);
static void mnt_pin(Mount *m) {
    ext2_pin_inode(m->root);
    ext2_pin_inode(m->mp);
}
static void mnt_unpin(Mount *m) {
    ext2_unpin_inode(m->root);
    ext2_unpin_inode(m->mp);
}

static MntNs *mntns_of(const Process *p) { return p && p->mntns ? p->mntns : &g_init_mnt; }
static MntNs *cur_mnt(void) {
    Thread *t = sched_current();
    /* the kernel's own file work (sandbox_real_root): the real root, and
       none of the calling process's mounts either */
    if (!t || t->fs_real_root) return &g_init_mnt;
    return mntns_of(t->proc);
}

/* 0: more mounts stacked there than are followed (the lookup fails rather
   than show what one of them covers) */
#define MNT_STACK 16
uint32_t mnt_enter(uint32_t ino, const void **ctx) {
    MntNs *ns = cur_mnt();
    if (!ns->mounts) return ino;
    /* stacked mounts: each mount crossed once, the newest first */
    const Mount *used[MNT_STACK];
    int nu = 0;
    for (bool again = true; again; ) {
        again = false;
        for (Mount *m = ns->mounts; m; m = m->next) {
            if (m->mp != ino) continue;
            bool seen = false;
            for (int k = 0; k < nu; k++) if (used[k] == m) seen = true;
            if (seen) continue;
            if (nu == MNT_STACK) return 0;
            used[nu++] = m;
            ino = m->root;
            *ctx = m;
            again = true;
            break;
        }
    }
    return ino;
}

bool mnt_leave(uint32_t ino, const void **ctx, uint32_t *parent) {
    const Mount *m = (const Mount *)*ctx;
    if (!m || m->root != ino) return false;
    *parent = m->mp_parent;
    *ctx = m->in;
    return true;
}

bool mnt_ro(const void *ctx) { return ctx && ((const Mount *)ctx)->ro; }

bool mnt_any_ro(void) {
    for (Mount *m = cur_mnt()->mounts; m; m = m->next) if (m->ro) return true;
    return false;
}

bool mnt_is_point(uint32_t ino) {
    for (Mount *m = cur_mnt()->mounts; m; m = m->next) if (m->mp == ino) return true;
    return false;
}

uint32_t mnt_up(uint32_t ino) {
    for (Mount *m = cur_mnt()->mounts; m; m = m->next)
        if (m->root == ino && m->mp != ino) return m->mp;
    return ino;
}

/* ---- tmpfs directories ---- */

static void tmp_unref(Tmpfs *t);
#define REAP_MAX 64
static char g_reap[REAP_MAX][40];
static int g_nreap;
static uint32_t g_tmp_seq;
static bool g_tmp_cleaned;

extern void *g_vfs_root_override;

/* as root, at the real root (whoever asks, wherever its root is) */
static void kernel_fs(bool on) {
    g_vfs_root_override = on ? (void *)sched_current() : NULL;
    sandbox_real_root(on);
}

/* everything under (and with) path; a few levels of directories */
static char g_rm_names[64][MAX_FILENAME];
static int g_rm_n;
static bool g_rm_dir[64];
static void rm_collect(VfsNode *n) {
    if (!n->name[0] || (n->name[0] == '.' && (!n->name[1] || (n->name[1] == '.' && !n->name[2])))) return;
    if (g_rm_n >= 64) return;
    int k = 0;
    while (n->name[k] && k < MAX_FILENAME - 1) { g_rm_names[g_rm_n][k] = n->name[k]; k++; }
    g_rm_names[g_rm_n][k] = 0;
    g_rm_dir[g_rm_n] = n->is_dir;
    g_rm_n++;
}
static void rm_rf(const char *path, int depth) {
    /* the thing itself first: a symlink (or a file) is removed, never
       followed -- a symlink to /etc among a tmpfs's files must not take
       /etc's contents with it */
    VfsStat st;
    if (depth > 16 || !vfs_lstat(path, &st)) return;
    if (st.is_link || !st.is_dir) { vfs_delete(path); return; }
    for (int round = 0; round < 64; round++) {
        g_rm_n = 0;
        if (!vfs_readdir(path, rm_collect) || g_rm_n == 0) break;
        int n = g_rm_n;
        /* whole names, our own copy: the recursion reuses the buffer */
        char (*names)[MAX_FILENAME] = kmalloc((uint64_t)n * MAX_FILENAME);
        if (!names) break;
        for (int k = 0; k < n; k++) memcpy(names[k], g_rm_names[k], MAX_FILENAME);
        bool removed = false;
        for (int k = 0; k < n; k++) {
            char child[MAX_PATH];
            uint32_t pl = 0, nl2 = 0;
            while (path[pl] && pl < MAX_PATH - 2) { child[pl] = path[pl]; pl++; }
            child[pl++] = '/';
            while (names[k][nl2] && pl + nl2 < MAX_PATH - 1) { child[pl + nl2] = names[k][nl2]; nl2++; }
            if (names[k][nl2]) continue;                  /* too long: left alone */
            child[pl + nl2] = 0;
            VfsStat cs;
            bool had = vfs_lstat(child, &cs);
            rm_rf(child, depth + 1);
            if (had && !vfs_lstat(child, &cs)) removed = true;
        }
        kfree(names);
        if (!removed) break;
    }
    vfs_delete(path);
}

static Tmpfs *tmp_new(Process *owner, uint32_t mode) {
    kernel_fs(true);
    VfsStat st;
    if (!vfs_stat("/.tmpfs", &st)) {
        vfs_mkdir("/.tmpfs");
        vfs_chmod("/.tmpfs", 0700);
    }
    Tmpfs *t = (Tmpfs *)kmalloc(sizeof(Tmpfs));
    if (!t) { kernel_fs(false); return NULL; }
    memset(t, 0, sizeof(*t));
    const char *pre = "/.tmpfs/";
    uint32_t n = 0;
    while (pre[n]) { t->path[n] = pre[n]; n++; }
    for (int tries = 0; tries < 1000; tries++) {
        char d[12];
        int k = 0;
        uint32_t m = n;
        uint32_t v = ++g_tmp_seq;
        do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (k) t->path[m++] = d[--k];
        t->path[m] = 0;
        if (!vfs_stat(t->path, &st)) break;     /* a fresh name */
    }
    bool ok = vfs_mkdir(t->path) && vfs_chown(t->path, owner->uid, owner->gid, false) == 0 &&
              vfs_chmod(t->path, mode & 07777);
    kernel_fs(false);
    if (!ok) { kfree(t); return NULL; }
    t->refs = 1;
    return t;
}

static void tmp_unref(Tmpfs *t) {
    if (!t || --t->refs) return;
    /* the flusher removes it (here we may be in the scheduler's reaper) */
    if (g_nreap < REAP_MAX) {
        int k = 0;
        while (t->path[k]) { g_reap[g_nreap][k] = t->path[k]; k++; }
        g_reap[g_nreap][k] = 0;
        g_nreap++;
    }
    kfree(t);
}

/* the flusher thread (nothing else runs rm_rf: its buffers are static) */
void sandbox_mnt_reap(void) {
    if (!g_tmp_cleaned) {                 /* tmpfs directories left from before a reboot */
        g_tmp_cleaned = true;
        kernel_fs(true);
        g_rm_n = 0;
        if (vfs_readdir("/.tmpfs", rm_collect)) {
            rm_rf("/.tmpfs", 0);
            vfs_mkdir("/.tmpfs");
            vfs_chmod("/.tmpfs", 0700);
        }
        kernel_fs(false);
    }
    while (g_nreap > 0) {
        char p[40];
        int i = --g_nreap, k = 0;
        while (g_reap[i][k]) { p[k] = g_reap[i][k]; k++; }
        p[k] = 0;
        kernel_fs(true);
        rm_rf(p, 0);
        kernel_fs(false);
    }
}

/* ---- namespaces ---- */

static void mnt_free_list(Mount *m) {
    while (m) {
        Mount *n = m->next;
        tmp_unref(m->tmp);
        mnt_unpin(m);
        kfree(m);
        m = n;
    }
}

static void mntns_unref(MntNs *ns) {
    if (!ns || ns == &g_init_mnt || --ns->refs) return;
    mnt_free_list(ns->mounts);
    kfree(ns);
}

/* a copy of `from`'s mounts (CLONE_NEWNS), owned by `owner`'s
   capabilities; inherited into a less privileged namespace, they are
   locked (cannot be unmounted, nor made writable) */
static MntNs *mntns_copy(MntNs *from, UserNs *owner) {
    MntNs *ns = (MntNs *)kmalloc(sizeof(MntNs));
    if (!ns) return NULL;
    memset(ns, 0, sizeof(*ns));
    ns->refs = 1;
    ns->id = g_ns_ids++;
    ns->owner = owner;
    bool lock = owner != from->owner;
    /* all of them (MNT_MAX at most), same order; `in` pointers mapped to the copies */
    Mount **olds = kmalloc(MNT_MAX * sizeof(Mount *)), **news = kmalloc(MNT_MAX * sizeof(Mount *));
    if (!olds || !news) { if (olds) kfree(olds); if (news) kfree(news); kfree(ns); return NULL; }
    int n = 0;
    Mount **tail = &ns->mounts;
    for (Mount *m = from->mounts; m; m = m->next) {
        Mount *c = n < MNT_MAX ? (Mount *)kmalloc(sizeof(Mount)) : NULL;
        if (!c) { mnt_free_list(ns->mounts); kfree(ns); kfree(olds); kfree(news); return NULL; }
        *c = *m;
        c->next = NULL;
        if (lock) c->locked = true;
        if (c->tmp) c->tmp->refs++;
        mnt_pin(c);
        olds[n] = m;
        news[n] = c;
        n++;
        *tail = c;
        tail = &c->next;
    }
    for (int i = 0; i < n; i++) {
        if (!news[i]->in) continue;
        Mount *mapped = NULL;
        for (int j = 0; j < n; j++) if (news[i]->in == olds[j]) { mapped = news[j]; break; }
        news[i]->in = mapped;             /* (every ancestor was copied: never a stale pointer) */
    }
    ns->count = (uint32_t)n;
    kfree(olds);
    kfree(news);
    return ns;
}

#define MS_RDONLY      1u
#define MS_REMOUNT     32u
#define MS_BIND        4096u
#define MS_MOVE        8192u
#define MS_REC         16384u
#define MS_UNBINDABLE  (1u << 17)
#define MS_PRIVATE     (1u << 18)
#define MS_SLAVE       (1u << 19)
#define MS_SHARED      (1u << 20)
#define MS_PROPAGATION (MS_UNBINDABLE | MS_PRIVATE | MS_SLAVE | MS_SHARED)

/* who may mount: CAP_SYS_ADMIN -- in a mount namespace of its own (a user
   namespace gives it), or as the real root in the initial one */
static bool may_mount(const Process *p) {
    /* CAP_SYS_ADMIN in the user namespace that owns its mount namespace */
    return has_cap(p, CAP_SYS_ADMIN) && mntns_of(p)->owner == p->userns;
}

static bool streq(const char *a, const char *b);

/* the inode at path, the mount the lookup ended in, and the inode of its
   directory */
static bool resolve(const char *path, uint32_t *ino, const void **ctx, uint32_t *parent, bool *is_dir) {
    VfsStat st;
    if (!vfs_stat(path, &st)) return false;
    *ino = st.first_cluster;
    *is_dir = st.is_dir;
    *ctx = sched_current()->lookup_mnt;
    if (parent) {
        char dir[MAX_PATH];
        uint32_t n = 0, last = 0;
        while (path[n] && n < MAX_PATH - 1) { dir[n] = path[n]; if (path[n] == '/') last = n; n++; }
        dir[last ? last : 1] = 0;
        VfsStat ds;
        if (!vfs_stat(dir, &ds)) return false;
        *parent = ds.first_cluster;
        /* the mountpoint's own context, not its directory's */
        if (!vfs_stat(path, &st)) return false;
        *ctx = sched_current()->lookup_mnt;
    }
    return true;
}

static int64_t add_mount(uint32_t mp, uint32_t mp_parent, uint32_t root, const void *in, bool ro, Tmpfs *tmp) {
    Process *p = cur();
    MntNs *ns = mntns_of(p);
    if (ns->count >= MNT_MAX) return -28;                   /* -ENOSPC */
    Mount *m = (Mount *)kmalloc(sizeof(Mount));
    if (!m) return -ENOMEM;
    memset(m, 0, sizeof(*m));
    m->mp = mp;
    m->mp_parent = mp_parent;
    m->root = root;
    m->in = (Mount *)in;
    m->ro = ro;
    m->tmp = tmp;
    m->next = ns->mounts;
    ns->mounts = m;
    ns->count++;
    mnt_pin(m);
    return 0;
}

int64_t sandbox_mount(const char *src, const char *tgt, const char *type, uint64_t flags) {
    Process *p = cur();
    if (!may_mount(p)) return -EPERM;
    uint32_t tino, tpar;
    const void *tctx;
    bool tdir;
    /* a symlink as the last component: its target's directory is not the
       one the textual path names (mounted there, ".." led anywhere) */
    VfsStat lst;
    if (vfs_lstat(tgt, &lst) && lst.is_link) return -EINVAL;
    if (!resolve(tgt, &tino, &tctx, &tpar, &tdir)) return -2;   /* -ENOENT */

    /* propagation only: accepted, nothing propagates */
    if ((flags & MS_PROPAGATION) && !(flags & (MS_BIND | MS_REMOUNT))) return 0;

    if (flags & MS_REMOUNT) {
        /* the mount whose root is the target: read-only or not */
        const Mount *m = (const Mount *)tctx;
        if (m && m->root == tino) {
            if (m->locked && m->ro && !(flags & MS_RDONLY)) return -EPERM;   /* inherited read-only */
            ((Mount *)m)->ro = (flags & MS_RDONLY) != 0;
            return 0;
        }
        return 0;                       /* the filesystem itself: nothing to change */
    }
    if (flags & MS_MOVE) return -EINVAL;

    if (flags & MS_BIND) {
        uint32_t sino;
        const void *sctx;
        bool sdir;
        if (!src || !resolve(src, &sino, &sctx, NULL, &sdir)) return -2;
        if (sdir != tdir) return sdir ? -20 : -21;          /* -ENOTDIR / -EISDIR */
        /* a read-only source stays read-only where it is bound again; a
           part of a tmpfs keeps that tmpfs */
        bool ro = (flags & MS_RDONLY) != 0 || mnt_ro(sctx);
        Tmpfs *tmp = NULL;
        for (const Mount *c = (const Mount *)sctx; c && !tmp; c = c->in) tmp = c->tmp;
        if (tmp) tmp->refs++;
        int64_t r = add_mount(tino, tpar, sino, tctx, ro, tmp);
        if (r && tmp) tmp_unref(tmp);
        return r;
    }

    if (!type) return -EINVAL;
    if (streq(type, "tmpfs") || streq(type, "ramfs")) {
        if (!tdir) return -20;
        Tmpfs *t = tmp_new(p, 0755);
        if (!t) return -ENOMEM;
        VfsStat st;
        kernel_fs(true);
        bool ok = vfs_stat(t->path, &st);
        kernel_fs(false);
        if (!ok) { tmp_unref(t); return -ENOMEM; }
        int64_t r = add_mount(tino, tpar, st.first_cluster, tctx, (flags & MS_RDONLY) != 0, t);
        if (r) tmp_unref(t);
        return r;
    }
    /* filesystems whose files exist by name everywhere here */
    if (streq(type, "proc") || streq(type, "sysfs") || streq(type, "devpts") || streq(type, "mqueue") ||
        streq(type, "cgroup") || streq(type, "cgroup2") || streq(type, "devtmpfs") ||
        streq(type, "securityfs") || streq(type, "debugfs") || streq(type, "binfmt_misc"))
        return tdir ? 0 : -20;
    return -19;                                              /* -ENODEV */
}

#define MNT_DETACH 2

int64_t sandbox_umount(const char *tgt, int flags) {
    (void)flags;
    Process *p = cur();
    if (!may_mount(p)) return -EPERM;
    uint32_t ino;
    const void *ctx;
    bool dir;
    if (!resolve(tgt, &ino, &ctx, NULL, &dir)) return -2;
    Mount *m = (Mount *)ctx;
    if (!m || m->root != ino) return -EINVAL;               /* not a mountpoint */
    if (m->locked) return -EINVAL;                          /* inherited: stays */
    MntNs *ns = mntns_of(p);
    /* it, and (detached) every mount inside it */
    bool removed = true;
    while (removed) {
        removed = false;
        for (Mount **pp = &ns->mounts; *pp; pp = &(*pp)->next) {
            Mount *x = *pp;
            bool inside = x == m;
            for (Mount *c = x->in; c && !inside; c = c->in) if (c == m) inside = true;
            if (!inside || x == m) continue;    /* the mounts inside it first */
            *pp = x->next;
            tmp_unref(x->tmp);
            mnt_unpin(x);
            kfree(x);
            ns->count--;
            removed = true;
            break;
        }
    }
    for (Mount **pp = &ns->mounts; *pp; pp = &(*pp)->next) {
        if (*pp != m) continue;
        *pp = m->next;
        tmp_unref(m->tmp);
        mnt_unpin(m);
        kfree(m);
        ns->count--;
        break;
    }
    return 0;
}

/* /proc/self/mountinfo (or /proc/self/mounts): the root and every mount
   of the caller's namespace it can reach, at their paths from its root */
static uint32_t put_s(char *o, uint32_t n, uint32_t cap, const char *s) {
    while (*s && n + 1 < cap) o[n++] = *s++;
    return n;
}
static uint32_t put_n(char *o, uint32_t n, uint32_t cap, uint32_t v) {
    char d[12];
    int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k && n + 1 < cap) o[n++] = d[--k];
    return n;
}

uint32_t sandbox_mountinfo(char *out, uint32_t cap, bool mountinfo) {
    Process *p = cur();
    MntNs *ns = mntns_of(p);
    uint32_t root = sandbox_lookup_root();
    uint32_t n = 0;
    /* oldest first: the list is newest first */
    Mount *order[128];
    int cnt = 0;
    for (Mount *m = ns->mounts; m && cnt < 128; m = m->next) order[cnt++] = m;
    /* the filesystem itself, unless a mount is the root */
    bool root_is_mount = false;
    for (int i = 0; i < cnt; i++) if (order[i]->root == root) root_is_mount = true;
    if (!root_is_mount) {
        n = put_s(out, n, cap, mountinfo ? "1 0 8:1 / / rw,relatime - ext2 /dev/root rw\n"
                                         : "/dev/root / ext2 rw,relatime 0 0\n");
    }
    for (int i = cnt - 1; i >= 0; i--) {
        Mount *m = order[i];
        char where[MAX_PATH];
        if (m->root == root) {
            where[0] = '/'; where[1] = 0;
        } else {
            char dir[MAX_PATH], name[MAX_FILENAME];
            if (!vfs_dir_path(m->mp_parent, dir, sizeof(dir))) continue;     /* not reachable from here */
            if (!vfs_name_in_dir(m->mp_parent, m->mp, name, sizeof(name))) continue;
            uint32_t k = 0, j = 0;
            while (dir[k] && k < MAX_PATH - 2) { where[k] = dir[k]; k++; }
            if (k == 0 || where[k - 1] != '/') where[k++] = '/';
            while (name[j] && k < MAX_PATH - 1) where[k++] = name[j++];
            where[k] = 0;
        }
        /* what is mounted, as a path of the filesystem ("/" for a tmpfs) */
        char what[MAX_PATH];
        what[0] = '/'; what[1] = 0;
        if (!m->tmp) {
            sandbox_real_root(true);
            Process *self = p;
            MntNs *keep = self->mntns;
            self->mntns = NULL;                  /* the plain filesystem, no mounts */
            if (!vfs_dir_path(m->root, what, sizeof(what))) { what[0] = '/'; what[1] = 0; }
            self->mntns = keep;
            sandbox_real_root(false);
        }
        const char *opts = m->ro ? "ro,relatime" : "rw,relatime";
        const char *fs = m->tmp ? "tmpfs" : "ext2";
        const char *src = m->tmp ? "tmpfs" : "/dev/root";
        if (mountinfo) {
            int parent = 1;
            for (int j = cnt - 1; j >= 0; j--) if (order[j] == m->in) parent = cnt - j + 1;
            n = put_n(out, n, cap, (uint32_t)(cnt - i + 1));
            n = put_s(out, n, cap, " ");
            n = put_n(out, n, cap, (uint32_t)parent);
            n = put_s(out, n, cap, m->tmp ? " 0:20 " : " 8:1 ");
            n = put_s(out, n, cap, what);
            n = put_s(out, n, cap, " ");
            n = put_s(out, n, cap, where);
            n = put_s(out, n, cap, " ");
            n = put_s(out, n, cap, opts);
            n = put_s(out, n, cap, " - ");
            n = put_s(out, n, cap, fs);
            n = put_s(out, n, cap, " ");
            n = put_s(out, n, cap, src);
            n = put_s(out, n, cap, m->ro ? " ro\n" : " rw\n");
        } else {
            n = put_s(out, n, cap, src);
            n = put_s(out, n, cap, " ");
            n = put_s(out, n, cap, where);
            n = put_s(out, n, cap, " ");
            n = put_s(out, n, cap, fs);
            n = put_s(out, n, cap, " ");
            n = put_s(out, n, cap, opts);
            n = put_s(out, n, cap, " 0 0\n");
        }
    }
    out[n] = 0;
    return n;
}

int64_t sandbox_pivot_root(const char *new_root, const char *put_old) {
    Process *p = cur();
    if (!may_mount(p) || !p->mntns) return -EPERM;           /* only in a namespace of its own */
    uint32_t nino, oino, opar;
    const void *nctx, *octx;
    bool ndir, odir;
    if (!resolve(new_root, &nino, &nctx, NULL, &ndir) || !ndir) return -20;
    const Mount *nm = (const Mount *)nctx;
    if (!nm || nm->root != nino) return -EINVAL;             /* new_root must be a mount */
    if (!resolve(put_old, &oino, &octx, &opar, &odir) || !odir) return -20;
    uint32_t old_root = p->root_inum ? p->root_inum : 2;
    if (nino == old_root) return -16;                         /* -EBUSY */
    /* the old root goes under put_old ... */
    int64_t r = add_mount(oino, opar, old_root, octx, false, NULL);
    if (r) return r;
    /* ... and new_root is "/" for the namespace's processes still at the old one */
    for (Process *q = process_list_head(); q; q = q->next) {
        if (q->mntns != p->mntns || q->exited) continue;
        uint32_t qr = q->root_inum ? q->root_inum : 2;
        if (qr != old_root) continue;
        q->root_inum = nino == 2 ? 0 : nino;
        if (q == p) { q->cwd[0] = '/'; q->cwd[1] = 0; }
    }
    return 0;
}

/* =====================================================================
 * process life
 * ===================================================================== */

void sandbox_fork(Process *child, Process *parent) {
    child->seccomp = parent->seccomp;
    filter_ref(child->seccomp);
    child->seccomp_mode = parent->seccomp_mode;
    child->no_new_privs = parent->no_new_privs;
    child->caps_set = parent->caps_set;
    child->cap_eff = parent->cap_eff;
    child->cap_prm = parent->cap_prm;
    child->cap_inh = parent->cap_inh;
    child->root_inum = parent->root_inum;
    child->chrooted = parent->chrooted;
    child->userns = parent->userns;
    child->netns = parent->netns;
    child->mntns = parent->mntns;
    if (child->mntns) child->mntns->refs++;
    child->nondumpable = parent->nondumpable;
    /* the pid namespace: the parent's for children (unshare), or a new one
       (clone(CLONE_NEWPID)) */
    PidNs *ns = parent->pidns_child ? parent->pidns_child : parent->pidns;
    if (g_pend_pid) { ns = g_pend_pid; g_pend_pid = NULL; }   /* clone(CLONE_NEWPID) */
    child->pidns = ns;
    child->pidns_child = ns;
    pidns_register(child->pid, child);   /* room reserved by sandbox_fork_check() */
}

bool sandbox_exec_ignores_setid(const Process *p) {
    return p->no_new_privs || p->userns != NULL;
}

/* execve(): root (in its namespace) keeps every capability, anyone else
   loses them */
void sandbox_exec(Process *p) {
    if (!p->caps_set) return;
    bool root_here = p->userns ? uid_to_ns(p, p->uid) == 0 : p->uid == 0;
    if (root_here) { p->cap_eff = p->cap_prm; }
    else { p->cap_eff = p->cap_prm = 0; }
}

void sandbox_teardown(Process *p) {
    filter_unref(p->seccomp);
    p->seccomp = NULL;
    mntns_unref(p->mntns);
    p->mntns = NULL;
}

/* a signal to every thread of q, as kill(2) sends it: pending, sleepers woken */
static void send_sig(Process *q, int sig) {
    Thread *start = sched_get_thread_list();
    if (!start) return;
    Thread *t = start;
    int guard = 0;
    do {
        if (t->proc == q && t->state != THREAD_STATE_TERMINATED) {
            t->sig_pending |= 1ULL << sig;
            if (t->state == THREAD_STATE_BLOCKED && signal_wants_wake(t, sig)) sched_unblock(t, -4 /* EINTR */);
        }
        t = t->next;
    } while (t != start && ++guard < 100000);
}

void sandbox_on_exit(Process *p) {
    /* children that asked (prctl(PR_SET_PDEATHSIG)) learn their parent died
       -- bubblewrap's --die-with-parent */
    for (Process *q = process_list_head(); q; q = q->next)
        if (!q->exited && q->ppid == p->pid && q->pdeathsig > 0 && q->pdeathsig < 65) send_sig(q, q->pdeathsig);
    PidNs *ns = p->pidns;
    if (!ns || ns->init != p->pid || ns->dead) return;
    /* the init of a pid namespace died: everything in it (and below) gets
       SIGKILL -- pending, sleepers woken, as kill(2) sends it; marking the
       threads terminated outright freed ones still inside the kernel */
    ns->dead = true;
    Thread *start = sched_get_thread_list();
    if (!start) return;
    Thread *t = start;
    int guard = 0;
    do {
        Process *q = t->proc;
        if (q && q != p && !q->exited && t->state != THREAD_STATE_TERMINATED) {
            for (PidNs *n = q->pidns; n; n = n->parent) {
                if (n != ns) continue;
                t->sig_pending |= 1ULL << 9;
                if (t->state == THREAD_STATE_BLOCKED) sched_unblock(t, -4 /* EINTR */);
                break;
            }
        }
        t = t->next;
    } while (t != start && ++guard < 100000);
}

/* =====================================================================
 * chroot
 * ===================================================================== */

#define EXT2_ROOT 2

uint32_t sandbox_lookup_root(void) {
    Thread *t = sched_current();
    if (!t || !t->proc || t->fs_real_root || !t->proc->root_inum) return EXT2_ROOT;
    return t->proc->root_inum;
}

void sandbox_real_root(bool on) {
    Thread *t = sched_current();
    if (t) t->fs_real_root = on;
}

int64_t sandbox_chroot(const char *kpath) {
    Process *p = cur();
    if (!has_cap(p, CAP_SYS_CHROOT)) return -EPERM;
    VfsStat st;
    if (!vfs_stat(kpath, &st)) return -2;              /* -ENOENT */
    if (!st.is_dir) return -20;                        /* -ENOTDIR */
    if (!vfs_may_access(kpath, 1)) return -EACCES;
    p->root_inum = st.first_cluster == EXT2_ROOT ? 0 : st.first_cluster;
    p->chrooted = p->root_inum != 0;
    /* the working directory is its "/" now (Linux leaves it outside, the
       classic way out; every sandbox follows chroot() with chdir("/")) */
    p->cwd[0] = '/';
    p->cwd[1] = 0;
    return 0;
}

/* =====================================================================
 * /proc/self/{uid_map,gid_map,setgroups}, /proc/self/ns/NAME
 * ===================================================================== */

static bool streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static bool streq_prefix(const char *s, const char *pre) { while (*pre) if (*s++ != *pre++) return false; return true; }

/* /proc/{self,thread-self,PID}/{uid_map,gid_map,setgroups}: the kind, and
   the process (global pid) it is about -- a PID of the caller's pid
   namespace (bubblewrap writes its child's maps from outside) */
int sandbox_proc_file(const char *path, uint64_t *gpid) {
    const char *pre = "/proc/";
    int i = 0;
    while (pre[i]) { if (path[i] != pre[i]) return 0; i++; }
    const char *q = path + i;
    Process *me = cur();
    uint64_t target = me->pid;
    if (q[0] == 's' && q[1] == 'e' && q[2] == 'l' && q[3] == 'f' && q[4] == '/') q += 5;
    else if (streq_prefix(q, "thread-self/")) q += 12;
    else if (*q >= '0' && *q <= '9') {
        uint64_t v = 0;
        while (*q >= '0' && *q <= '9') { v = v * 10 + (uint64_t)(*q - '0'); if (v > 0xFFFFFFFFull) return 0; q++; }
        if (*q != '/') return 0;
        q++;
        target = pid_from_ns(v, me);
        if (!target) return 0;
    } else {
        return 0;
    }
    int kind = streq(q, "uid_map") ? 1 : streq(q, "gid_map") ? 2 : streq(q, "setgroups") ? 3 : 0;
    if (kind) *gpid = target;
    return kind;
}

static uint32_t put_u(char *o, uint32_t n, uint32_t cap, uint32_t v, uint32_t width) {
    char d[12];
    int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (uint32_t w = (uint32_t)k; w < width && n + 1 < cap; w++) o[n++] = ' ';
    while (k && n + 1 < cap) o[n++] = d[--k];
    return n;
}

/* /proc/self, /proc/thread-self, /proc/PID (visible to the caller), and
   their ns/ and fd/: directories without inodes */
bool sandbox_proc_dir(const char *path) {
    const char *pre = "/proc/";
    int i = 0;
    while (pre[i]) { if (path[i] != pre[i]) return false; i++; }
    const char *q = path + i;
    if (streq_prefix(q, "self")) q += 4;
    else if (streq_prefix(q, "thread-self")) q += 11;
    else if (*q >= '0' && *q <= '9') {
        uint64_t v = 0;
        while (*q >= '0' && *q <= '9') { v = v * 10 + (uint64_t)(*q - '0'); if (v > 0xFFFFFFFFull) return false; q++; }
        if (!pid_from_ns(v, cur())) return false;
    } else {
        return false;
    }
    while (*q == '/') q++;
    if (!*q) return true;
    if (streq_prefix(q, "ns") || streq_prefix(q, "fd")) {
        q += 2;
        while (*q == '/') q++;
        return !*q;
    }
    return false;
}

int64_t sandbox_proc_read(int kind, uint64_t gpid, char *out, uint32_t cap) {
    Process *p = process_find_by_pid(gpid);
    if (!p) return -ESRCH;
    const UserNs *ns = p->userns;
    uint32_t n = 0;
    if (kind == 3) {
        const char *s = (ns && ns->setgroups_deny) ? "deny\n" : "allow\n";
        while (*s && n + 1 < cap) out[n++] = *s++;
    } else if (!ns) {
        /* the initial namespace: everything, identity */
        n = put_u(out, n, cap, 0, 10); n = put_u(out, n, cap, 0, 11); n = put_u(out, n, cap, 4294967295u, 11);
        if (n + 1 < cap) out[n++] = '\n';
    } else if (kind == 1 ? ns->uid_set : ns->gid_set) {
        n = put_u(out, n, cap, kind == 1 ? ns->uid_in : ns->gid_in, 10);
        n = put_u(out, n, cap, kind == 1 ? ns->uid_out : ns->gid_out, 11);
        n = put_u(out, n, cap, kind == 1 ? ns->uid_cnt : ns->gid_cnt, 11);
        if (n + 1 < cap) out[n++] = '\n';
    }
    out[n] = 0;
    return n;
}

static bool parse_u(const char **s, uint32_t *v) {
    while (**s == ' ' || **s == '\t') (*s)++;
    if (**s < '0' || **s > '9') return false;
    uint64_t x = 0;
    while (**s >= '0' && **s <= '9') { x = x * 10 + (uint64_t)(**s - '0'); if (x > 0xFFFFFFFFull) return false; (*s)++; }
    *v = (uint32_t)x;
    return true;
}

/* Writing a map: once; one line "inside outside count". A writer with
   CAP_SETUID (CAP_SETGID for gid_map) in the parent namespace -- the real
   root, or root of the namespace the new one was made from -- may map any
   ids that exist there. Anyone else exactly one id: its own euid (egid,
   and only after "deny" went to setgroups) as the parent namespace sees
   it. */
int64_t sandbox_proc_write(int kind, uint64_t gpid, const char *buf, uint32_t len) {
    Process *w = cur();                            /* who writes */
    Process *p = process_find_by_pid(gpid);        /* whose namespace */
    if (!p || p->exited) return -ESRCH;
    UserNs *ns = p->userns;
    /* the process itself, or one in the namespace its user namespace was
       made from, as the namespace's creator (or the real root) */
    if (p != w && !(ns && ns->parent == w->userns && (w->uid == ns->owner || has_cap(w, CAP_SETUID))))
        return -EPERM;
    char t[64];
    if (len >= sizeof(t)) return -EINVAL;
    memcpy(t, buf, len);
    t[len] = 0;
    if (kind == 3) {
        if (!ns) return -EPERM;
        if (streq(t, "deny") || streq(t, "deny\n")) {
            if (ns->gid_set) return -EPERM;
            ns->setgroups_deny = true;
            return len;
        }
        if (streq(t, "allow") || streq(t, "allow\n")) return ns->setgroups_deny ? -EPERM : (int64_t)len;
        return -EINVAL;
    }
    if (!ns) return -EPERM;                        /* the initial maps are fixed */
    if (kind == 1 ? ns->uid_set : ns->gid_set) return -EPERM;
    const char *s = t;
    uint32_t in, out, cnt;
    if (!parse_u(&s, &in) || !parse_u(&s, &out) || !parse_u(&s, &cnt)) return -EINVAL;
    while (*s == ' ' || *s == '\n') s++;
    if (*s) return -EINVAL;                        /* one line only */
    if (cnt == 0 || (uint64_t)in + cnt > 0xFFFFFFFFull || (uint64_t)out + cnt > 0xFFFFFFFFull) return -EINVAL;
    bool privileged = w->userns == ns->parent && has_cap(w, kind == 1 ? CAP_SETUID : CAP_SETGID);
    if (privileged) {
        /* the outside ids must exist in the parent namespace */
        if (id_up(ns->parent, out, kind == 2) == NOID || id_up(ns->parent, out + cnt - 1, kind == 2) == NOID)
            return -EPERM;
    } else {
        if (cnt != 1) return -EPERM;
        if (kind == 1 && out != id_down(ns->parent, w->uid, false)) return -EPERM;
        if (kind == 2) {
            if (!ns->setgroups_deny) return -EPERM;
            if (out != id_down(ns->parent, w->gid, true)) return -EPERM;
        }
    }
    if (kind == 1) { ns->uid_in = in; ns->uid_out = out; ns->uid_cnt = cnt; ns->uid_set = true; }
    else           { ns->gid_in = in; ns->gid_out = out; ns->gid_cnt = cnt; ns->gid_set = true; }
    return len;
}

bool sandbox_ns_link(const char *name, char *out, uint32_t cap) {
    Process *p = cur();
    /* Linux's initial namespaces' inode numbers; ours count up from 0xF0000000 */
    uint32_t base;
    uint32_t own = 0;
    if (streq(name, "user"))   { base = 4026531837u; own = p->userns ? p->userns->id : 0; }
    else if (streq(name, "pid") || streq(name, "pid_for_children")) {
        base = 4026531836u;
        const PidNs *ns = streq(name, "pid") ? p->pidns : (p->pidns_child ? p->pidns_child : p->pidns);
        own = ns ? ns->id : 0;
    }
    else if (streq(name, "net"))    { base = 4026531840u; own = p->netns ? 0x8000u + p->netns : 0; }
    else if (streq(name, "uts"))    base = 4026531838u;
    else if (streq(name, "ipc"))    base = 4026531839u;
    else if (streq(name, "mnt"))    { base = 4026531841u; own = p->mntns ? p->mntns->id : 0; }
    else if (streq(name, "cgroup")) base = 4026531835u;
    else if (streq(name, "time") || streq(name, "time_for_children")) base = 4026531834u;
    else return false;
    uint32_t ino = own ? 0xF0000000u + own : base;
    const char *nm = name;
    if (streq(name, "pid_for_children")) nm = "pid";
    if (streq(name, "time_for_children")) nm = "time";
    uint32_t n = 0;
    while (*nm && n + 1 < cap) out[n++] = *nm++;
    const char *mid = ":[";
    while (*mid && n + 1 < cap) out[n++] = *mid++;
    n = put_u(out, n, cap, ino, 0);
    if (n + 1 < cap) out[n++] = ']';
    out[n] = 0;
    return true;
}
