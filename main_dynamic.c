/*
 * WynlandOS - Dynamically Linked Executable for PT_INTERP Validation
 */

#define SYS_write 1
#define SYS_exit 60

static void sys_write(const char *str) {
    int len = 0;
    while (str[len]) len++;
    __asm__ volatile(
        "movq $1, %%rax\n"
        "movq $1, %%rdi\n"
        "movq %0, %%rsi\n"
        "movq %1, %%rdx\n"
        "syscall\n"
        :
        : "r"(str), "r"((long)len)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
}

static void sys_exit(int code) {
    __asm__ volatile(
        "movq $60, %%rax\n"
        "movq %0, %%rdi\n"
        "syscall\n"
        :
        : "r"((long)code)
        : "rax", "rdi", "rcx", "r11"
    );
}

void _start(void) {
    sys_write("--> Main Binary: Dynamic main executable started!\n");
    sys_exit(0);
}
