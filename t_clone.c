#define SYS_write 1
#define SYS_exit  60
#define SYS_clone 56

/* Forward declarations */
void _start(void);
long sys_write(int fd, const void *buf, int size);
long my_clone(void (*fn)(void), void *stack_top, long flags);
void child_thread_entry(void);

char child_stack[8192];
volatile int child_done = 0;

void _start(void) {
    const char *msg1 = "Starting userspace SYS_clone test...\n";
    int len1 = 0;
    while (msg1[len1]) len1++;
    sys_write(1, msg1, len1);

    /* Setup clone flags */
    long flags = 0x00000100 | 0x00000200 | 0x00000400 | 0x00000800 | 0x00010000;
    
    /* Call my_clone. Child starts at child_thread_entry, using child_stack */
    long ret = my_clone(child_thread_entry, &child_stack[8192], flags);

    if (ret > 0) {
        /* Parent thread continues here */
        const char *pmsg = "Hello from parent thread! Spawned child TID: ";
        int plen = 0;
        while (pmsg[plen]) plen++;
        sys_write(1, pmsg, plen);

        /* Print TID */
        char tid_str[16];
        int idx = 0;
        long temp = ret;
        if (temp == 0) {
            tid_str[idx++] = '0';
        } else {
            char rev[16];
            int r_idx = 0;
            while (temp > 0) {
                rev[r_idx++] = '0' + (temp % 10);
                temp /= 10;
            }
            while (r_idx > 0) {
                tid_str[idx++] = rev[--r_idx];
            }
        }
        tid_str[idx++] = '\n';
        sys_write(1, tid_str, idx);

        /* Wait for child thread to finish */
        while (!child_done) {
            /* Yield CPU using standard sched_yield (24) */
            __asm__ volatile(
                "movq $24, %%rax\n"
                "syscall\n"
                ::: "rax", "rcx", "r11"
            );
        }

        const char *exit_msg = "Parent thread verified child thread completion! Test passed.\n";
        int elen = 0;
        while (exit_msg[elen]) elen++;
        sys_write(1, exit_msg, elen);

        /* Exit parent thread */
        __asm__ volatile(
            "movq $60, %%rax\n"
            "movq $0, %%rdi\n"
            "syscall\n"
            ::: "rax", "rdi"
        );
    } else {
        const char *emsg = "SYS_clone failed!\n";
        int elen = 0;
        while (emsg[elen]) elen++;
        sys_write(1, emsg, elen);
        
        /* Exit parent thread with failure */
        __asm__ volatile(
            "movq $60, %%rax\n"
            "movq $1, %%rdi\n"
            "syscall\n"
            ::: "rax", "rdi"
        );
    }
}

void child_thread_entry(void) {
    const char *cmsg = "Hello from spawned child thread running in Ring 3!\n";
    int clen = 0;
    while (cmsg[clen]) clen++;
    sys_write(1, cmsg, clen);
    
    child_done = 1;
}

long my_clone(void (*fn)(void), void *stack_top, long flags) {
    long ret;
    __asm__ volatile(
        // 1. Prepare child stack: push fn onto the child stack
        "subq $16, %[stack]\n"
        "movq %[fn], (%[stack])\n"
        
        // 2. Call SYS_clone(flags, child_stack)
        "movq %[sys_clone], %%rax\n"
        "movq %[flags], %%rdi\n"
        "movq %[stack], %%rsi\n"
        "syscall\n"
        
        // 3. Check if parent or child
        "testq %%rax, %%rax\n"
        "jnz .parent\n"
        
        // Child thread:
        "xorq %%rbp, %%rbp\n"    // Clear rbp for clean callstack
        "popq %%rax\n"           // Pop fn pointer into rax
        "call *%%rax\n"          // Call fn()
        "movq $0, %%rdi\n"       // Exit code 0
        "movq %[sys_exit], %%rax\n"
        "syscall\n"              // Exit child thread
        
        ".parent:\n"
        "movq %%rax, %[ret]\n"
        : [ret] "=r"(ret)
        : [fn] "r"(fn), [stack] "r"(stack_top), [flags] "r"(flags),
          [sys_clone] "i"(56), [sys_exit] "i"(60)
        : "rax", "rcx", "r11", "rdi", "rsi", "memory"
    );
    return ret;
}

long sys_write(int fd, const void *buf, int size) {
    long ret;
    __asm__ volatile(
        "movq %[sys_write], %%rax\n"
        "movq %[fd], %%rdi\n"
        "movq %[buf], %%rsi\n"
        "movq %[size], %%rdx\n"
        "syscall\n"
        : "=a"(ret)
        : [sys_write] "i"(1), [fd] "g"((long)fd), [buf] "g"((long)buf), [size] "g"((long)size)
        : "rcx", "r11", "rdi", "rsi", "rdx", "memory"
    );
    return ret;
}
