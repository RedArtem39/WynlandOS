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
    if (flags & CLONE_NEWNS) return -EINVAL;             /* mount namespaces: not yet */
    if (flags & CLONE_NEWUSER) {
        if (p->root_inum) return -EPERM;                 /* not from inside a chroot */
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

int64_t sandbox_clone_check(uint64_t flags) {
    g_pend_user = NULL;
    g_pend_pid = NULL;
    if (!(flags & NS_FLAGS)) return 0;
    Process *p = cur();
    if (flags & CLONE_NEWUSER) {
        if (p->root_inum) return -EPERM;
        if ((p->userns ? p->userns->level : 0) >= NS_MAX_LEVEL) return -EUSERS;
        if (flags & CLONE_NEWNS) return -EINVAL;
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
    return 0;
}

/* the fork failed: what was made for it goes */
void sandbox_clone_abort(void) {
    if (g_pend_user) kfree(g_pend_user);
    if (g_pend_pid) kfree(g_pend_pid);
    g_pend_user = NULL;
    g_pend_pid = NULL;
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
    child->userns = parent->userns;
    child->netns = parent->netns;
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
}

void sandbox_on_exit(Process *p) {
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

int sandbox_proc_file(const char *path) {
    if (streq(path, "/proc/self/uid_map") || streq(path, "/proc/thread-self/uid_map")) return 1;
    if (streq(path, "/proc/self/gid_map") || streq(path, "/proc/thread-self/gid_map")) return 2;
    if (streq(path, "/proc/self/setgroups") || streq(path, "/proc/thread-self/setgroups")) return 3;
    return 0;
}

static uint32_t put_u(char *o, uint32_t n, uint32_t cap, uint32_t v, uint32_t width) {
    char d[12];
    int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (uint32_t w = (uint32_t)k; w < width && n + 1 < cap; w++) o[n++] = ' ';
    while (k && n + 1 < cap) o[n++] = d[--k];
    return n;
}

int64_t sandbox_proc_read(int kind, char *out, uint32_t cap) {
    Process *p = cur();
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

/* Writing a map: once; one line "inside outside count". Unprivileged (no
   CAP_SETUID/CAP_SETGID over the parent namespace -- here: not root of
   the initial namespace) a process may map exactly one id: its own euid
   (egid, and only after "deny" went to setgroups) as seen from the
   parent namespace. */
int64_t sandbox_proc_write(int kind, const char *buf, uint32_t len) {
    Process *p = cur();
    UserNs *ns = p->userns;
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
    bool privileged = !ns->parent && p->uid == 0 && ns->owner == 0;
    if (!privileged) {
        if (cnt != 1) return -EPERM;
        if (kind == 1 && out != id_down(ns->parent, p->uid, false)) return -EPERM;
        if (kind == 2) {
            if (!ns->setgroups_deny) return -EPERM;
            if (out != id_down(ns->parent, p->gid, true)) return -EPERM;
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
    else if (streq(name, "mnt"))    base = 4026531841u;
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
