/*
 * WynlandOS - Dummy Dynamic Linker / Interpreter for PT_INTERP Validation
 */

#define SYS_write 1
#define SYS_exit 60

#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_FLAGS   8
#define AT_ENTRY   9
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_SECURE  23
#define AT_RANDOM  25

typedef struct {
    unsigned long a_type;
    unsigned long a_val;
} AuxvEntry;

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
    sys_write("--> Dummy Linker: Started successfully!\n");

    /*
     * According to System V ABI, the stack at _start points to:
     * rsp -> argc
     * rsp+8 -> argv[0]
     * ...
     * NULL after argv
     * envp
     * NULL after envp
     * auxv
     */
    register unsigned long *rsp __asm__("rsp");
    unsigned long argc = *rsp;
    unsigned long *argv = rsp + 1;
    (void)argc;

    sys_write("--> Dummy Linker: Parsing stack frame...\n");

    /* Skip argv */
    unsigned int idx = 0;
    while (argv[idx] != 0) {
        idx++;
    }
    idx++; // Skip NULL

    /* Skip envp */
    unsigned long *envp = &argv[idx];
    idx = 0;
    while (envp[idx] != 0) {
        idx++;
    }
    idx++; // Skip NULL

    /* Now we are at auxv */
    AuxvEntry *auxv = (AuxvEntry *)&envp[idx];
    unsigned long entry_addr = 0;

    for (int i = 0; auxv[i].a_type != AT_NULL; i++) {
        if (auxv[i].a_type == AT_ENTRY) {
            entry_addr = auxv[i].a_val;
        }
    }

    if (entry_addr == 0) {
        sys_write("--> Dummy Linker: ERROR - AT_ENTRY not found in auxv!\n");
        sys_exit(1);
    }

    sys_write("--> Dummy Linker: Found main entry point. Jumping to main executable...\n");

    /* Jump to main program entry point */
    void (*entry_func)(void) = (void (*)(void))entry_addr;
    entry_func();
}
