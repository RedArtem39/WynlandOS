/*
 * WynlandOS - Phase 1 fd-inheritance verification binary (spawner side)
 * Creates a pipe, marks the READ end close-on-exec (so it must NOT leak
 * into the child) and leaves the WRITE end inheritable, then spawns
 * inherited.elf via the new SYS_spawn syscall. inherited.elf never calls
 * pipe() itself -- if it can still write through the SAME fd number and
 * spawner reads it back, fd inheritance is proven end-to-end.
 * Freestanding, no libc, raw syscalls -- same style as proc_a.c/t_clone.c.
 * Link at a high, non-aliased address (see Phase 0's stack/ET_DYN fixes
 * for why low addresses are unsafe): -Ttext 0x300000000000.
 */

#define SYS_read   0
#define SYS_write  1
#define SYS_pipe   22
#define SYS_yield  24
#define SYS_exit   60
#define SYS_fcntl  72
#define SYS_spawn  405

#define F_SETFD    2
#define FD_CLOEXEC 1

static long sys3(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile(
        "movq %1, %%rax\n"
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r"(ret)
        : "r"(num), "r"(a1), "r"(a2), "r"(a3)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

static void sys_write(int fd, const char *buf, long len) { sys3(SYS_write, fd, (long)buf, len); }
static long sys_read(int fd, char *buf, long len) { return sys3(SYS_read, fd, (long)buf, len); }
static long sys_pipe(int *pfd) { return sys3(SYS_pipe, (long)pfd, 0, 0); }
static void sys_yield(void) { sys3(SYS_yield, 0, 0, 0); }
static void sys_exit(int code) { sys3(SYS_exit, code, 0, 0); }
static long sys_fcntl(int fd, int cmd, long arg) { return sys3(SYS_fcntl, fd, cmd, arg); }
static long sys_spawn(const char *path) { return sys3(SYS_spawn, (long)path, 0, 0); }

static void print_num(int fd, long n) {
    char buf[12];
    int i = 0;
    if (n == 0) { sys_write(fd, "0", 1); return; }
    char rev[12];
    int r = 0;
    while (n > 0) { rev[r++] = '0' + (n % 10); n /= 10; }
    while (r > 0) buf[i++] = rev[--r];
    sys_write(fd, buf, i);
}

void _start(void) {
    sys_write(1, "spawner: start\n", 15);

    int pfd[2];
    sys_pipe(pfd);

    sys_write(1, "spawner: pipe read_fd=", 22);
    print_num(1, pfd[0]);
    sys_write(1, " write_fd=", 10);
    print_num(1, pfd[1]);
    sys_write(1, "\n", 1);

    /* Read end must NOT leak into the child -- mark it close-on-exec.
       Write end is left inheritable (default: fd_flags starts at 0,
       i.e. not CLOEXEC) so inherited.elf can use it. */
    sys_fcntl(pfd[0], F_SETFD, FD_CLOEXEC);

    long pid = sys_spawn("/inherited.elf");
    if (pid < 0) {
        sys_write(1, "spawner: SYS_spawn failed\n", 26);
        sys_exit(1);
    }
    sys_write(1, "spawner: spawned inherited.elf\n", 32);

    char buf[32];
    long n = 0;
    for (int i = 0; i < 200; i++) {
        n = sys_read(pfd[0], buf, sizeof(buf));
        if (n > 0) break;
        sys_yield();
    }

    if (n > 0) {
        sys_write(1, "spawner: got back [", 20);
        sys_write(1, buf, n);
        sys_write(1, "]\n", 2);
    } else {
        sys_write(1, "spawner: got nothing back -- inheritance FAILED\n", 49);
    }

    sys_exit(0);
}
