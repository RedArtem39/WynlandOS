/*
 * WynlandOS - Phase 4 verification binary (SHM + argv, parent side)
 * Creates a SHM segment (SYS_shm_create, 407), mmaps it, writes a known
 * pattern into it, then spawns shm_test_child.elf via SYS_spawn_argv (408)
 * passing the SHM fd number as argv[1]. If the child (which never calls
 * SYS_shm_create itself) can read back the SAME pattern through its own
 * mmap of the inherited fd, both new primitives -- argv delivery and
 * true zero-copy shared memory -- are proven end-to-end.
 * Freestanding, no libc, raw syscalls -- same style as spawner.c. Kept as
 * a permanent regression binary (not wired into `make image`), matching
 * spawner.c/inherited.c's convention -- rerun manually if future changes
 * to process_spawn()/elf_load()/SYS_mmap's SHM branch need re-verifying.
 *
 * Build (must be ET_EXEC, non-PIE -- musl-cross gcc defaults to PIE even
 * with -static; a PIE binary gets elf_load()'s ET_DYN load_offset
 * (0x500000000000) added on top of -Ttext, landing segments somewhere
 * never actually mapped. -Ttext-segment, not plain -Ttext, is also
 * required -- plain -Ttext only repositions .text, leaving
 * .note.gnu.property at the linker's default low base (0x400000),
 * which collides with kernel identity-mapped memory and gets refused by
 * load_elf_segments()'s vmm_is_user_page() collision check):
 *   x86_64-linux-musl-gcc -nostdlib -static -no-pie -fno-pie \
 *     -mcmodel=large -O2 -Wl,-Ttext-segment=0x310000000000 \
 *     -o shm_test_parent.elf shm_test_parent.c
 */

#define SYS_write     1
#define SYS_mmap      9
#define SYS_yield     24
#define SYS_exit      60
#define SYS_shm_create 407
#define SYS_spawn_argv 408

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
static void sys_yield(void) { sys3(SYS_yield, 0, 0, 0); }
static void sys_exit(int code) { sys3(SYS_exit, code, 0, 0); }
static long sys_shm_create(long size) { return sys3(SYS_shm_create, size, 0, 0); }
static void *sys_mmap(long fd) {
    return (void *)sys6(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
}
static long sys_spawn_argv(const char *path, const char **argv) {
    return sys3(SYS_spawn_argv, (long)path, (long)argv, 0);
}

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void num_to_str(long n, char *out) {
    char rev[12];
    int r = 0;
    if (n == 0) { out[0] = '0'; out[1] = '\0'; return; }
    while (n > 0) { rev[r++] = '0' + (n % 10); n /= 10; }
    int i = 0;
    while (r > 0) out[i++] = rev[--r];
    out[i] = '\0';
}

void _start(void) {
    sys_write(1, "[shm_test_parent] start\n", 25);

    long shm_fd = sys_shm_create(4096);
    if (shm_fd < 0) {
        sys_write(1, "[shm_test_parent] SYS_shm_create failed\n", 41);
        sys_exit(1);
    }

    char *shm = (char *)sys_mmap(shm_fd);
    if ((long)shm <= 0) {
        sys_write(1, "[shm_test_parent] mmap failed\n", 31);
        sys_exit(1);
    }

    const char *pattern = "SHM-ZEROCOPY-OK";
    int plen = str_len(pattern);
    for (int i = 0; i <= plen; i++) shm[i] = pattern[i];

    sys_write(1, "[shm_test_parent] wrote pattern into shm, shm_fd=", 50);
    char numbuf[12];
    num_to_str(shm_fd, numbuf);
    sys_write(1, numbuf, str_len(numbuf));
    sys_write(1, "\n", 1);

    const char *argv[3];
    argv[0] = "/shm_test_child.elf";
    argv[1] = numbuf;
    argv[2] = (const char *)0;

    long pid = sys_spawn_argv("/shm_test_child.elf", argv);
    if (pid < 0) {
        sys_write(1, "[shm_test_parent] SYS_spawn_argv failed\n", 41);
        sys_exit(1);
    }
    sys_write(1, "[shm_test_parent] spawned shm_test_child.elf\n", 47);

    for (int i = 0; i < 500; i++) sys_yield();

    sys_write(1, "[shm_test_parent] done\n", 24);
    sys_exit(0);
}
