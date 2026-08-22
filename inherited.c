/*
 * WynlandOS - Phase 1 fd-inheritance verification binary (child side)
 * Deliberately never calls pipe() itself -- writes a fixed tag directly
 * through the fd number it expects to have inherited from spawner.c
 * (spawner.c's write end of its pipe). If this shows up on spawner's
 * read end, fd inheritance across SYS_spawn works end-to-end.
 * INHERITED_WRITE_FD below must match what spawner.c's own diagnostic
 * print reports as its write_fd (see spawner.c's serial output).
 */

#define SYS_write 1
#define SYS_exit  60

#define INHERITED_WRITE_FD 4

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
static void sys_exit(int code) { sys3(SYS_exit, code, 0, 0); }

void _start(void) {
    sys_write(1, "inherited: start, writing through inherited fd\n", 49);
    sys_write(INHERITED_WRITE_FD, "INHERITED-OK", 12);
    sys_write(1, "inherited: done\n", 17);
    sys_exit(0);
}
