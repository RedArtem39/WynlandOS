/*
 * WynlandOS - Phase 0 process-isolation verification binary (A side)
 * Creates its own pipe, writes/reads back a distinct tag, loops with
 * sched_yield() so the scheduler interleaves with proc_b.elf if both
 * are running concurrently. Freestanding, no libc, raw syscalls --
 * same style as t_clone.c / main_dynamic.c.
 */

#define SYS_read  0
#define SYS_write 1
#define SYS_pipe  22
#define SYS_yield 24
#define SYS_exit  60

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

static void print_num(int fd, int n) {
    char buf[12];
    int i = 0;
    if (n == 0) {
        sys_write(fd, "0", 1);
        return;
    }
    char rev[12];
    int r = 0;
    while (n > 0) {
        rev[r++] = '0' + (n % 10);
        n /= 10;
    }
    while (r > 0) buf[i++] = rev[--r];
    sys_write(fd, buf, i);
}

void _start(void) {
    int pfd[2];
    sys_pipe(pfd);

    const char *tag = "PROC-A";
    sys_write(pfd[1], tag, 6);

    char readback[16];
    long n = sys_read(pfd[0], readback, sizeof(readback));

    for (int i = 1; i <= 5; i++) {
        sys_write(1, "[", 1);
        sys_write(1, readback, n);
        sys_write(1, "] iter ", 7);
        print_num(1, i);
        sys_write(1, "\n", 1);
        sys_yield();
    }

    sys_exit(0);
}
