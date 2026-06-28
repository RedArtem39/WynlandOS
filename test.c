#define SYS_yield   0
#define SYS_exit    2
#define SYS_read    12
#define SYS_write   13

/* Forward declarations */
void _start(void);
long syscall_raw(long num, long a1, long a2, long a3);
void sys_exit(int code);
long sys_write(int fd, const void *buf, int size);
long sys_read(int fd, void *buf, int size);

void _start() {
    // 1. Write startup message
    const char *msg1 = "Hello from userspace C program test!\n";
    int len1 = 0;
    while (msg1[len1]) len1++;
    sys_write(1, msg1, len1);

    // 2. Ask user to press a key
    const char *msg2 = "Please type a key: ";
    int len2 = 0;
    while (msg2[len2]) len2++;
    sys_write(1, msg2, len2);

    // 3. Read a key from stdin (fd 0)
    char input_char = 0;
    sys_read(0, &input_char, 1);

    // 4. Write back the received key
    const char *msg3 = "\nYou typed key: ";
    int len3 = 0;
    while (msg3[len3]) len3++;
    sys_write(1, msg3, len3);

    char echo_buf[2] = {input_char, '\n'};
    sys_write(1, echo_buf, 2);

    // 5. Exit cleanly
    sys_exit(0);
}

long syscall_raw(long num, long a1, long a2, long a3) {
    long ret;
    register long r_num __asm__("rax") = num;
    register long r_a1  __asm__("rdi") = a1;
    register long r_a2  __asm__("rsi") = a2;
    register long r_a3  __asm__("rdx") = a3;
    
    __asm__ volatile(
        "syscall\n"
        : "=a"(ret)
        : "0"(r_num), "r"(r_a1), "r"(r_a2), "r"(r_a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

void sys_exit(int code) {
    syscall_raw(SYS_exit, code, 0, 0);
    while (1) {
        __asm__ volatile("hlt");
    }
}

long sys_write(int fd, const void *buf, int size) {
    return syscall_raw(SYS_write, fd, (long)buf, size);
}

long sys_read(int fd, void *buf, int size) {
    return syscall_raw(SYS_read, fd, (long)buf, size);
}
