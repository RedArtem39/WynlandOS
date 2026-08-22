/*
 * WynlandOS - Phase 4 verification binary (SHM + argv, child side)
 * Never calls SYS_shm_create itself -- reads the shm fd number out of
 * argv[1] (delivered via the new SYS_spawn_argv, 408), mmaps that
 * INHERITED fd, and checks the pattern shm_test_parent.c wrote is
 * visible with zero copying through the kernel. Proves argv delivery
 * (elf.c's new stack-building path) and real SYS_mmap MAP_SHARED
 * zero-copy (kernel/syscall.c's 0xFFFFFFFD SHM branch) together.
 *
 * Freestanding: _start is a raw asm trampoline that grabs argc/argv off
 * the initial stack per the SysV ABI process-entry layout (real ELF
 * entry gets these on the stack, not in registers -- there's no musl
 * crt0 here to do this for us, same reason spawner.c/inherited.c don't
 * use a normal main()).
 * Permanent regression binary (see shm_test_parent.c's header for the
 * full build-flag rationale -- must be built the same way, ET_EXEC via
 * -no-pie -mcmodel=large -Wl,-Ttext-segment=..., not plain -Ttext):
 *   x86_64-linux-musl-gcc -nostdlib -static -no-pie -fno-pie \
 *     -mcmodel=large -O2 -Wl,-Ttext-segment=0x320000000000 \
 *     -o shm_test_child.elf shm_test_child.c
 */

#define SYS_write 1
#define SYS_mmap  9
#define SYS_exit  60

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define MAP_SHARED 0x1

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

static long sys6(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile(
        "movq %1, %%rax\n"
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r"(ret)
        : "r"(num), "r"(a1), "r"(a2), "r"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

static void sys_write(int fd, const char *buf, long len) { sys3(SYS_write, fd, (long)buf, len); }
static void sys_exit(int code) { sys3(SYS_exit, code, 0, 0); }
static void *sys_mmap(long fd) {
    return (void *)sys6(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
}

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static long str_to_long(const char *s) {
    long n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return n;
}

void real_start(long argc, char **argv) {
    sys_write(1, "[shm_test_child] start, argc=", 30);
    char numbuf[12];
    /* reuse str_to_long's inverse is unnecessary -- just print argc via a
       tiny local itoa since num_to_str isn't shared across these two
       standalone freestanding files */
    {
        long n = argc; int i = 0; char rev[12]; int r = 0;
        if (n == 0) { numbuf[0]='0'; numbuf[1]=0; }
        else { while (n > 0) { rev[r++] = '0' + (n % 10); n /= 10; } while (r>0) numbuf[i++]=rev[--r]; numbuf[i]=0; }
    }
    sys_write(1, numbuf, str_len(numbuf));
    sys_write(1, "\n", 1);

    if (argc < 2) {
        sys_write(1, "[shm_test_child] FAIL: no argv[1] (shm fd) received\n", 54);
        sys_exit(1);
    }

    long shm_fd = str_to_long(argv[1]);
    sys_write(1, "[shm_test_child] parsed shm_fd=", 32);
    sys_write(1, argv[1], str_len(argv[1]));
    sys_write(1, "\n", 1);

    char *shm = (char *)sys_mmap(shm_fd);
    if ((long)shm <= 0) {
        sys_write(1, "[shm_test_child] FAIL: mmap of inherited shm fd failed\n", 57);
        sys_exit(1);
    }

    const char *expected = "SHM-ZEROCOPY-OK";
    if (str_eq(shm, expected)) {
        sys_write(1, "[shm_test_child] PASS: read back [", 35);
        sys_write(1, shm, str_len(expected));
        sys_write(1, "] via zero-copy shared memory\n", 30);
    } else {
        sys_write(1, "[shm_test_child] FAIL: pattern mismatch, got [", 47);
        sys_write(1, shm, str_len(expected) < 64 ? str_len(expected) : 64);
        sys_write(1, "]\n", 2);
    }

    sys_exit(0);
}

__asm__(
    ".global _start\n"
    "_start:\n"
    "    movq (%rsp), %rdi\n"
    "    leaq 8(%rsp), %rsi\n"
    "    andq $-16, %rsp\n"
    "    call real_start\n"
);
