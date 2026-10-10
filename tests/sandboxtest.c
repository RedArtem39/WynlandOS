/*
 * WynlandOS - sandboxing check (kernel/sandbox.c): seccomp-bpf,
 * no_new_privs, capabilities, chroot, user/pid/network namespaces -- used
 * the way browsers' sandboxes use them. Runs as an ordinary user (the
 * test account); every test that changes the process runs in a child.
 *
 * Copyright (C) 2026 Red_Artem39. GPL-2.0-or-later.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <grp.h>
#include <ucontext.h>
#include <unistd.h>

static int pass, fail;

static void check(int ok, const char *what) {
    printf("[sandboxtest] %s %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
    if (ok) pass++; else fail++;
}

/* run fn in a child; its exit code (0 = good) decides, the name says what */
static int in_child(int (*fn)(void)) {
    pid_t c = fork();
    if (c == 0) { fflush(stdout); _exit(fn()); }
    int st = 0;
    if (waitpid(c, &st, 0) != c) return -1;
    if (WIFSIGNALED(st)) return 1000 + WTERMSIG(st);
    return WEXITSTATUS(st);
}

/* a filter: one syscall gets `action`, everything else is allowed */
static int filter_one(int nr, unsigned action) {
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, action),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = { sizeof(f) / sizeof(f[0]), f };
    return syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog);
}

/* ---------------------------------------------------------------- seccomp */

static int t_needs_nnp(void) {
    /* without no_new_privs an unprivileged process may not filter */
    if (filter_one(SYS_getppid, SECCOMP_RET_ERRNO | EPERM) == 0) return 1;
    if (errno != EACCES) return 2;
    if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 0) return 3;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return 4;
    if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) return 5;
    return 0;
}

static int t_errno(void) {
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    if (filter_one(SYS_getppid, SECCOMP_RET_ERRNO | 77) != 0) return 1;
    if (syscall(SYS_getppid) != -1 || errno != 77) return 2;
    if (getpid() <= 0) return 3;                         /* the others still run */
    if (prctl(PR_GET_SECCOMP, 0, 0, 0, 0) != 2) return 4;
    return 0;
}

static volatile int trap_ok;
static void on_sigsys(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    trap_ok = sig == SIGSYS && si->si_code == 1 /* SYS_SECCOMP */ && si->si_syscall == SYS_getppid &&
              si->si_arch == AUDIT_ARCH_X86_64 && si->si_errno == 12 &&
              si->si_call_addr == (void *)uc->uc_mcontext.gregs[REG_RIP] &&
              uc->uc_mcontext.gregs[REG_RAX] == SYS_getppid;
    uc->uc_mcontext.gregs[REG_RAX] = 4242;               /* the emulated syscall's result */
}

static int t_trap(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_sigsys;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSYS, &sa, NULL);
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    if (filter_one(SYS_getppid, SECCOMP_RET_TRAP | 12) != 0) return 1;
    long r = syscall(SYS_getppid);
    if (r != 4242) return 2;
    if (!trap_ok) return 3;
    return 0;
}

static int t_kill(void) {
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    if (filter_one(SYS_getppid, SECCOMP_RET_KILL_PROCESS) != 0) return 1;
    syscall(SYS_getppid);
    return 2;                                            /* not reached */
}

static int t_strict(void) {
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT, 0, 0, 0) != 0) return 1;
    if (syscall(SYS_write, 1, "", 0) != 0) syscall(SYS_exit, 2);
    syscall(SYS_getpid);                                 /* not allowed: SIGKILL */
    syscall(SYS_exit, 3);
    return 3;
}

static int t_stack(void) {
    /* two filters: the stricter answer wins (ERRNO 5 over ERRNO 6, both
       over ALLOW), and both stay after fork() and execve() */
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    if (filter_one(SYS_getppid, SECCOMP_RET_ERRNO | 6) != 0) return 1;
    if (filter_one(SYS_getppid, SECCOMP_RET_TRAP) != 0) return 2;
    if (filter_one(SYS_getppid, SECCOMP_RET_ERRNO | 5) != 0) return 3;
    signal(SIGSYS, SIG_DFL);
    /* TRAP beats ERRNO: the process dies of SIGSYS */
    pid_t c = fork();
    if (c == 0) { syscall(SYS_getppid); _exit(9); }
    int st;
    waitpid(c, &st, 0);
    if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGSYS) return 4;
    /* across execve: the program checks it itself */
    c = fork();
    if (c == 0) { execl("/sandboxtest.elf", "sandboxtest", "--filtered", (char *)NULL); _exit(7); }
    waitpid(c, &st, 0);
    if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGSYS) return 5;
    return 0;
}

static int t_badfilter(void) {
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    struct sock_filter f1[] = { BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 3) };            /* unaligned, no RET */
    struct sock_filter f2[] = { BPF_JUMP(BPF_JMP | BPF_JA, 5, 0, 0), BPF_STMT(BPF_RET | BPF_K, 0) };
    struct sock_filter f3[] = { BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 64), BPF_STMT(BPF_RET | BPF_K, 0) };
    struct sock_filter f4[] = { BPF_STMT(BPF_ALU | BPF_DIV | BPF_K, 0), BPF_STMT(BPF_RET | BPF_K, 0) };
    struct sock_fprog p1 = { 1, f1 }, p2 = { 2, f2 }, p3 = { 2, f3 }, p4 = { 2, f4 }, p0 = { 0, f1 };
    struct sock_fprog *ps[] = { &p1, &p2, &p3, &p4, &p0 };
    for (int i = 0; i < 5; i++)
        if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, ps[i]) != -1 || errno != EINVAL) return 10 + i;
    unsigned act = SECCOMP_RET_TRAP;
    if (syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &act) != 0) return 2;
    act = SECCOMP_RET_USER_NOTIF;
    if (syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &act) != -1) return 3;
    return 0;
}

/* --------------------------------------------------------- user namespace */

static int write_file(const char *path, const char *s) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    int n = write(fd, s, strlen(s));
    close(fd);
    return n == (int)strlen(s) ? 0 : -1;
}

/* a new user namespace in which we are root, mapped to our own ids */
static int become_ns_root(void) {
    uid_t u = getuid();
    gid_t g = getgid();
    if (unshare(CLONE_NEWUSER) != 0) return -1;
    char m[64];
    snprintf(m, sizeof m, "0 %u 1", (unsigned)u);
    if (write_file("/proc/self/uid_map", m) != 0) return -2;
    if (write_file("/proc/self/setgroups", "deny") != 0) return -3;
    snprintf(m, sizeof m, "0 %u 1", (unsigned)g);
    if (write_file("/proc/self/gid_map", m) != 0) return -4;
    return 0;
}

static int t_userns(void) {
    uid_t outer = getuid();
    char before[64] = {0}, after[64] = {0};
    readlink("/proc/self/ns/user", before, sizeof before - 1);
    if (unshare(CLONE_NEWUSER) != 0) return 1;
    if (getuid() != 65534) return 2;                     /* nothing mapped yet */
    char m[64];
    snprintf(m, sizeof m, "0 %u 2", (unsigned)outer);
    if (write_file("/proc/self/uid_map", m) == 0) return 3;   /* more than our own id: no */
    snprintf(m, sizeof m, "0 %u 1", (unsigned)outer);
    if (write_file("/proc/self/uid_map", m) != 0) return 4;
    if (write_file("/proc/self/uid_map", m) == 0) return 5;   /* only once */
    if (write_file("/proc/self/gid_map", "0 0 1") == 0) return 6;   /* not before setgroups=deny */
    if (write_file("/proc/self/setgroups", "deny") != 0) return 7;
    if (getuid() != 0 || geteuid() != 0) return 8;
    readlink("/proc/self/ns/user", after, sizeof after - 1);
    if (!strcmp(before, after) || strncmp(after, "user:[", 6)) return 9;
    char rb[64] = {0};
    int fd = open("/proc/self/uid_map", O_RDONLY);
    if (fd < 0 || read(fd, rb, sizeof rb - 1) <= 0) return 10;
    close(fd);
    unsigned a, b, c;
    if (sscanf(rb, "%u %u %u", &a, &b, &c) != 3 || a != 0 || b != outer || c != 1) return 11;
    /* root in here is not root of the system */
    if (open("/etc/sandboxtest-x", O_WRONLY | O_CREAT, 0644) >= 0) return 12;
    if (setuid(0) != 0) return 13;                       /* our own id, mapped */
    if (setuid(5) == 0) return 14;                       /* not mapped */
    gid_t none[1] = { 0 };
    if (setgroups(1, none) == 0) return 15;              /* setgroups is denied */
    return 0;
}

static int t_userns_caps(void) {
    if (become_ns_root() != 0) return 1;
    struct { unsigned v; int pid; } h = { 0x20080522, 0 };
    unsigned d[6];
    if (syscall(SYS_capget, &h, d) != 0) return 2;
    if (!(d[0] & (1u << 21))) return 3;                  /* CAP_SYS_ADMIN, in the namespace */
    memset(d, 0, sizeof d);
    if (syscall(SYS_capset, &h, d) != 0) return 4;       /* drop everything */
    if (chroot("/tmp") == 0) return 5;                   /* no CAP_SYS_CHROOT any more */
    if (unshare(CLONE_NEWNET) == 0) return 6;            /* nor CAP_SYS_ADMIN */
    return 0;
}

/* ---------------------------------------------------------- pid namespace */

static int t_pidns(void) {
    if (become_ns_root() != 0) return 1;
    pid_t outer_parent = getppid();
    if (unshare(CLONE_NEWPID) != 0) return 2;
    if (getpid() <= 1) return 3;                         /* we stay where we are */
    int pfd[2];
    pipe(pfd);
    pid_t c = fork();
    if (c == 0) {
        int ok = getpid() == 1 && getppid() == 0;
        /* the parent's world is not visible from here */
        ok = ok && kill(outer_parent, 0) == -1 && errno == ESRCH;
        /* a child of ours is pid 2, and we wait for it by that number */
        pid_t g = fork();
        if (g == 0) _exit(getpid() == 2 ? 0 : 1);
        int st;
        ok = ok && g == 2 && waitpid(2, &st, 0) == 2 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
        char r = ok ? 'y' : 'n';
        write(pfd[1], &r, 1);
        _exit(0);
    }
    char r = 0;
    read(pfd[0], &r, 1);
    int st;
    if (waitpid(c, &st, 0) != c) return 4;               /* we see it by its outer pid */
    if (r != 'y') return 5;
    /* clone(CLONE_NEWPID) the same way */
    return 0;
}

/* --------------------------------------------------------- net namespace */

static int t_netns(void) {
    /* an abstract socket outside... */
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path, "\0sandboxtest", 12);
    socklen_t al = offsetof(struct sockaddr_un, sun_path) + 12;
    if (bind(srv, (struct sockaddr *)&a, al) != 0 || listen(srv, 1) != 0) return 1;
    if (become_ns_root() != 0) return 2;
    if (unshare(CLONE_NEWNET) != 0) return 3;
    /* ...is not reachable from in here */
    int cl = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(cl, (struct sockaddr *)&a, al) == 0) return 4;
    /* and there is no IP network */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in in;
    memset(&in, 0, sizeof in);
    in.sin_family = AF_INET;
    in.sin_port = htons(80);
    in.sin_addr.s_addr = htonl(0x01010101);
    if (s >= 0 && connect(s, (struct sockaddr *)&in, sizeof in) == 0) return 5;
    if (errno != ENETUNREACH) return 6;
    return 0;
}

/* ------------------------------------------------------------------ chroot */

static int t_chroot_needs_cap(void) {
    return chroot("/tmp") == -1 && errno == EPERM ? 0 : 1;
}

static int t_chroot(void) {
    char jail[64];
    snprintf(jail, sizeof jail, "/tmp/sandboxjail-%d", (int)getpid());
    char p[128];
    mkdir(jail, 0755);
    snprintf(p, sizeof p, "%s/inside", jail);
    int fd = open(p, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) return 1;
    write(fd, "in", 2);
    close(fd);
    snprintf(p, sizeof p, "%s/d", jail);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/esc", jail);
    symlink("/etc", p);                                  /* an absolute link: the jail's /etc */
    int outside = open("/", O_RDONLY | O_DIRECTORY);
    if (outside < 0) return 2;
    if (become_ns_root() != 0) return 3;
    if (chroot(jail) != 0) return 4;
    char cwd[64];
    if (!getcwd(cwd, sizeof cwd) || strcmp(cwd, "/")) return 5;
    if (access("/inside", R_OK) != 0) return 6;
    if (access("/etc/passwd", F_OK) == 0) return 7;
    if (access("/../../etc/passwd", F_OK) == 0) return 8;
    if (access("/d/../../../etc/passwd", F_OK) == 0) return 9;
    if (access("/esc/passwd", F_OK) == 0) return 10;
    if (chdir("/d") != 0 || access("../inside", R_OK) != 0 || access("../../etc/passwd", F_OK) == 0) return 11;
    /* a directory opened before: no way back out through it */
    if (openat(outside, "etc/passwd", O_RDONLY) >= 0) return 12;
    if (fchdir(outside) == 0) return 13;
    /* and no new user namespace from in here */
    if (unshare(CLONE_NEWUSER) == 0) return 14;
    /* the root's ".." is not a name to move or remove (it is the real
       parent's entry) */
    if (rename("/..", "/moved") == 0 || rmdir("/..") == 0) return 15;
    if (rename("/d/..", "/moved") == 0) return 16;
    return 0;
}

/* a regular file is not a directory to walk through, whatever its bytes
   say (made-up directory entries pointing at the real root) */
static int t_file_not_dir(void) {
    const char *f = "/tmp/sandboxtest-notadir";
    int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 1;
    /* an ext2 directory entry: inode 2 (the root), rec_len 12, name ".." */
    unsigned char ent[4096];
    memset(ent, 0, sizeof ent);
    ent[0] = 2; ent[4] = 12; ent[6] = 2; ent[7] = 2; ent[8] = '.'; ent[9] = '.';
    ent[12] = 2; ent[16] = 0xF4; ent[17] = 0x0F; ent[18] = 3; ent[19] = 2; ent[20] = 'e'; ent[21] = 't'; ent[22] = 'c';
    if (write(fd, ent, sizeof ent) != (int)sizeof ent) return 2;
    close(fd);
    int ok = access("/tmp/sandboxtest-notadir/etc/passwd", F_OK) != 0 &&
             access("/tmp/sandboxtest-notadir/..", F_OK) != 0;
    unlink(f);
    return ok ? 0 : 3;
}

/* classic BPF: dividing by a zero X ends the program with 0 -- a kill */
static int t_bpf_div0(void) {
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    struct sock_filter f[] = {
        BPF_STMT(BPF_LDX | BPF_W | BPF_IMM, 0),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_STMT(BPF_ALU | BPF_DIV | BPF_X, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = { 4, f };
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) != 0) return 1;
    syscall(SYS_getpid);
    return 2;
}

/* ------------------------------------------------------- mount namespace */

static char g_mt[64];       /* /tmp/sandboxmnt-PID, made by main */

static int put(const char *path, const char *s) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int n = write(fd, s, strlen(s));
    close(fd);
    return n == (int)strlen(s) ? 0 : -1;
}

static int got(const char *path, const char *s) {
    char b[64] = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    int n = read(fd, b, sizeof b - 1);
    close(fd);
    return n == (int)strlen(s) && !memcmp(b, s, n);
}

static int t_mount_needs_cap(void) {
    char b[96];
    snprintf(b, sizeof b, "%s/b", g_mt);
    return mount("tmpfs", b, "tmpfs", 0, NULL) == -1 && errno == EPERM ? 0 : 1;
}

static int t_bind(void) {
    char a[96], b[96], p[128];
    snprintf(a, sizeof a, "%s/a", g_mt);
    snprintf(b, sizeof b, "%s/b", g_mt);
    if (become_ns_root() != 0) return 1;
    if (unshare(CLONE_NEWNS) != 0) return 2;
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) return 3;   /* bwrap's first step */
    if (mount(a, b, NULL, MS_BIND | MS_REC, NULL) != 0) return 4;
    snprintf(p, sizeof p, "%s/f", b);
    if (!got(p, "hello")) return 5;                     /* a's file, seen at b */
    /* ".." of the mount's root is b's directory, not a's */
    struct stat s1, s2;
    snprintf(p, sizeof p, "%s/..", b);
    if (stat(p, &s1) != 0 || stat(g_mt, &s2) != 0 || s1.st_ino != s2.st_ino) return 6;
    char cwd[128];
    if (chdir(b) != 0 || !getcwd(cwd, sizeof cwd) || strcmp(cwd, b)) return 7;
    chdir("/");
    /* read-only */
    if (mount(NULL, b, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) != 0) return 8;
    snprintf(p, sizeof p, "%s/f", b);
    if (open(p, O_WRONLY) >= 0) return 9;
    snprintf(p, sizeof p, "%s/new", b);
    if (open(p, O_WRONLY | O_CREAT, 0644) >= 0 || mkdir(p, 0755) == 0) return 10;
    snprintf(p, sizeof p, "%s/g", a);                    /* the source itself stays writable */
    if (put(p, "x") != 0) return 11;
    unlink(p);
    if (rmdir(b) == 0) return 12;                        /* a mountpoint is busy */
    if (umount2(b, MNT_DETACH) != 0) return 13;
    snprintf(p, sizeof p, "%s/f", b);
    if (access(p, F_OK) == 0) return 14;                 /* b is b again */
    /* tmpfs */
    if (mount("tmpfs", b, "tmpfs", 0, "mode=0755") != 0) return 15;
    snprintf(p, sizeof p, "%s/t", b);
    if (put(p, "tmp") != 0 || !got(p, "tmp")) return 16;
    if (umount2(b, 0) != 0) return 17;
    if (access(p, F_OK) == 0) return 18;
    /* this is ours: the parent's view gets a mount for good, it must not see it */
    if (mount(a, b, NULL, MS_BIND, NULL) != 0) return 19;
    /* a hard link does not cross from one mount to another */
    char f1[128], f2[128];
    snprintf(f1, sizeof f1, "%s/f", b);
    snprintf(f2, sizeof f2, "%s/f-link", g_mt);
    if (link(f1, f2) == 0 || errno != EXDEV) return 20;
    /* a symlink is not a mount target (its directory is not the textual one) */
    snprintf(p, sizeof p, "%s/lnk", g_mt);
    symlink(a, p);
    if (mount(a, p, NULL, MS_BIND, NULL) == 0) return 21;
    return 0;
}

/* a directory without search permission is not walked through (root's
   home is 0700: nothing in it, whatever its name) */
static int t_search(void) {
    struct stat st;
    if (stat("/root", &st) != 0 || (st.st_mode & 0777) != 0700) return 0;   /* (no /root here) */
    if (stat("/root/.", &st) == 0) return 1;
    if (access("/root/.bashrc", F_OK) == 0 || errno != EACCES && errno != ENOENT) return 2;
    return 0;
}

/* what bubblewrap does: a tmpfs as the new root, the system's /usr in it
   read-only, pivot_root, the old root detached -- and a socket from
   outside bound in */
static int t_pivot(void) {
    char base[96], p[160], sock[128];
    snprintf(base, sizeof base, "%s/base", g_mt);
    snprintf(sock, sizeof sock, "%s/sock/s", g_mt);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, sock);
    if (bind(srv, (struct sockaddr *)&a, sizeof a) != 0 || listen(srv, 2) != 0) return 1;
    if (become_ns_root() != 0) return 2;
    if (unshare(CLONE_NEWNS) != 0) return 3;
    if (mount("tmpfs", base, "tmpfs", 0, NULL) != 0) return 4;
    snprintf(p, sizeof p, "%s/usr", base);  mkdir(p, 0755);
    if (mount("/usr", p, NULL, MS_BIND | MS_REC, NULL) != 0) return 5;
    if (mount(NULL, p, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) != 0) return 6;
    snprintf(p, sizeof p, "%s/run", base);  mkdir(p, 0755);
    char sockdir[128];
    snprintf(sockdir, sizeof sockdir, "%s/sock", g_mt);
    if (mount(sockdir, p, NULL, MS_BIND, NULL) != 0) return 7;
    snprintf(p, sizeof p, "%s/oldroot", base);  mkdir(p, 0755);
    if (chdir(base) != 0) return 8;
    if (syscall(SYS_pivot_root, ".", "oldroot") != 0) return 9;
    if (umount2("/oldroot", MNT_DETACH) != 0) return 10;
    if (chdir("/") != 0) return 11;
    if (access("/usr/bin/fish", X_OK) != 0) return 12;        /* the system's programs */
    if (access("/etc/passwd", F_OK) == 0) return 13;           /* nothing else */
    if (access("/usr/../etc/passwd", F_OK) == 0) return 14;
    if (access("/oldroot/etc/passwd", F_OK) == 0) return 15;
    if (open("/usr/sandboxtest-x", O_WRONLY | O_CREAT, 0644) >= 0) return 16;   /* read-only */
    if (put("/scratch", "ok") != 0) return 17;                  /* the tmpfs root is ours */
    char cwd[64];
    if (!getcwd(cwd, sizeof cwd) || strcmp(cwd, "/")) return 18;
    /* the socket from outside, through the bind mount */
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un in;
    memset(&in, 0, sizeof in);
    in.sun_family = AF_UNIX;
    strcpy(in.sun_path, "/run/s");
    if (connect(c, (struct sockaddr *)&in, sizeof in) != 0) return 19;
    /* a mount's root, held open: its path is where it is mounted
       (bubblewrap checks every mount this way) */
    int ufd = open("/usr", O_PATH | O_CLOEXEC);
    char lp[64], lk[128] = {0};
    snprintf(lp, sizeof lp, "/proc/self/fd/%d", ufd);
    if (ufd < 0) return 20;
    if (readlink(lp, lk, sizeof lk - 1) <= 0) { printf("[sandboxtest] readlink %s: %s\n", lp, strerror(errno)); return 21; }
    if (strcmp(lk, "/usr")) { printf("[sandboxtest] readlink %s = %s\n", lp, lk); return 22; }
    return 0;
}

/* the real bubblewrap from Ubuntu, if the image has it */
static int t_bwrap(void) {
    if (access("/usr/bin/bwrap", X_OK) != 0) return 0;
    int pfd[2];
    if (pipe(pfd) != 0) return 1;
    pid_t c = fork();
    if (c == 0) {
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        /* the libraries live in /lib64 here, not under /usr */
        execl("/usr/bin/bwrap", "bwrap", "--unshare-all", "--die-with-parent",
              "--ro-bind", "/usr", "/usr", "--ro-bind", "/lib64", "/lib64", "--ro-bind", "/lib", "/lib",
              "--symlink", "usr/bin", "/bin", "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp",
              "/usr/bin/sh", "-c", "test -e /etc/passwd || echo sandboxed-$(id -u)", (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    char out[512] = {0};
    int n = 0, r;
    while (n < (int)sizeof out - 1 && (r = read(pfd[0], out + n, sizeof out - 1 - n)) > 0) n += r;
    int st;
    waitpid(c, &st, 0);
    printf("[sandboxtest] bwrap said: %s (status %d)\n", out, WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st));
    if (!strstr(out, "sandboxed-")) return 2;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 3;
}

static int t_procfd(void) {
    char b[64];
    return readlink("/proc/self/fd/99999999999999", b, sizeof b) == -1 ? 0 : 1;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--filtered")) {
        /* exec'd with the filters of t_stack: getppid traps */
        syscall(SYS_getppid);
        return 9;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("[sandboxtest] running as uid %d\n", (int)getuid());
    check(in_child(t_needs_nnp) == 0, "seccomp: filters need no_new_privs");
    check(in_child(t_errno) == 0, "seccomp: RET_ERRNO fails one syscall, the rest run");
    check(in_child(t_trap) == 0, "seccomp: RET_TRAP -> SIGSYS, siginfo, result from the ucontext");
    check(in_child(t_kill) == 1000 + SIGSYS, "seccomp: RET_KILL_PROCESS kills with SIGSYS");
    check(in_child(t_strict) == 1000 + SIGKILL, "seccomp: strict mode");
    check(in_child(t_stack) == 0, "seccomp: stacked filters, inherited by fork and execve");
    check(in_child(t_badfilter) == 0, "seccomp: bad programs refused, GET_ACTION_AVAIL");
    check(in_child(t_userns) == 0, "user namespace: own id as root, maps once, no real privilege");
    check(in_child(t_userns_caps) == 0, "user namespace: capabilities, capset drops them");
    check(in_child(t_pidns) == 0, "pid namespace: init is 1, the outside invisible");
    check(in_child(t_netns) == 0, "network namespace: no IP, its own abstract sockets");
    check(in_child(t_chroot_needs_cap) == 0, "chroot: refused without the capability");
    check(in_child(t_chroot) == 0, "chroot: .., symlinks and old fds stay inside");
    check(in_child(t_file_not_dir) == 0, "paths: a file's bytes are not directory entries");
    check(in_child(t_bpf_div0) == 1000 + SIGSYS, "seccomp: division by a zero X kills");
    check(in_child(t_procfd) == 0, "/proc/self/fd/<huge>: no such fd, no crash");
    snprintf(g_mt, sizeof g_mt, "/tmp/sandboxmnt-%d", (int)getpid());
    {
        char p[128];
        mkdir(g_mt, 0755);
        snprintf(p, sizeof p, "%s/a", g_mt);    mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/a/f", g_mt);  put(p, "hello");
        snprintf(p, sizeof p, "%s/b", g_mt);    mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/base", g_mt); mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/sock", g_mt); mkdir(p, 0755);
    }
    check(in_child(t_mount_needs_cap) == 0, "mount: refused without the capability");
    check(in_child(t_search) == 0, "paths: no way through a directory without search permission");
    check(in_child(t_bind) == 0, "mount namespace: bind, read-only, .., getcwd, tmpfs, umount");
    {
        char p[128];
        snprintf(p, sizeof p, "%s/b/f", g_mt);
        check(access(p, F_OK) != 0, "mount namespace: its mounts are its own");
    }
    check(in_child(t_pivot) == 0, "mount namespace: tmpfs root, ro /usr, pivot_root, a socket bound in");
    check(in_child(t_bwrap) == 0, "bubblewrap runs a program in a sandbox");
    printf("[sandboxtest] DONE pass=%d fail=%d\n", pass, fail);
    return fail ? 1 : 0;
}
