/*
 * WynlandOS - JIT check (glibc, host-built)
 * ============================================================
 * Without WYNLAND_ALLOW_JIT: writable+executable memory is refused (W^X),
 * but a W^X JIT (write RW, then mprotect RX) works. The test then execs
 * itself with WYNLAND_ALLOW_JIT=1: RWX anonymous memory is allowed and
 * code generated into it runs and can be patched in place; file mappings
 * stay W^X. One "[jittest] PASS|FAIL" line per check.
 *
 * Build: gcc -O2 -o build/jittest.elf tests/jittest.c
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int g_pass, g_fail;
static void check(int ok, const char *what)
{
    fprintf(stderr, "[jittest] %s %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) g_pass++; else g_fail++;
}

/* mov eax, imm32 ; ret */
static void emit_ret(unsigned char *p, int v)
{
    p[0] = 0xB8;
    memcpy(p + 1, &v, 4);
    p[5] = 0xC3;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "rwx") == 0) {
        void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check(m != MAP_FAILED, "with WYNLAND_ALLOW_JIT=1: RWX anonymous mmap");
        if (m != MAP_FAILED) {
            emit_ret(m, 42);
            check(((int (*)(void))m)() == 42, "code generated into RWX memory runs");
            emit_ret(m, 43);   /* patch it while it is executable */
            check(((int (*)(void))m)() == 43, "RWX code patched in place");
            munmap(m, 4096);
        }
        void *rw = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check(rw != MAP_FAILED && mprotect(rw, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0,
              "mprotect to RWX on anonymous memory");
        int fd = open("/etc/hosts", O_RDONLY);
        if (fd >= 0) {
            void *f = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE, fd, 0);
            check(f == MAP_FAILED, "file mapping stays W^X even with the flag");
            close(fd);
        }
        fprintf(stderr, "[jittest] DONE pass=%d fail=%d\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }

    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(m == MAP_FAILED && errno == EACCES, "RWX refused without WYNLAND_ALLOW_JIT");

    /* a W^X JIT: write, then flip to read+execute */
    unsigned char *c = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(c != MAP_FAILED, "RW mapping for code");
    if (c != MAP_FAILED) {
        emit_ret(c, 7);
        check(mprotect(c, 4096, PROT_READ | PROT_EXEC) == 0, "mprotect RW -> RX");
        check(((int (*)(void))c)() == 7, "W^X JIT code runs");
    }

    /* phase 2: the same program, opted in */
    fprintf(stderr, "[jittest] phase 1 pass=%d fail=%d, re-exec with WYNLAND_ALLOW_JIT=1\n", g_pass, g_fail);
    if (g_fail) return 1;
    char *nargv[] = { argv[0], "rwx", NULL };
    char *nenv[] = { "WYNLAND_ALLOW_JIT=1", "LD_LIBRARY_PATH=/lib64", NULL };
    execve("/jittest.elf", nargv, nenv);
    check(0, "execve");
    return 1;
}
