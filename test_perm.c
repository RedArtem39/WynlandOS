/*
 * WynlandOS - Phase 5 verification binary: real UID + permission
 * enforcement + password-gated elevation.
 * Freestanding, no libc -- same style as spawner.c/shm_test_*.c.
 * Kept as a permanent regression binary (not wired into `make image`).
 * Build:
 *   x86_64-linux-musl-gcc -nostdlib -static -no-pie -fno-pie \
 *     -mcmodel=large -O2 -Wl,-Ttext-segment=0x370000000000 \
 *     -o test_perm.elf test_perm.c
 */
#include "zerp_syscalls.h"

#define O_WRONLY 1

static void zprint(const char *s) { zwrite(1, s, zstrlen(s)); }
static void zprint_long(long v) {
    char nb[16];
    if (v < 0) { zprint("-1"); return; }
    znum_to_str(v, nb);
    zprint(nb);
}

int zerp_main(int argc, char **argv) {
    (void)argc; (void)argv;

    zprint("[test_perm] uid before elevate: ");
    long uid = zgetuid();
    zprint_long(uid); zprint("\n");

    long fd = zopen("/etc/sudopw", O_WRONLY);
    zprint("[test_perm] write-open /etc/sudopw (not elevated) -> ");
    zprint_long(fd);
    zprint(fd < 0 ? " (EXPECTED: denied)\n" : " (UNEXPECTED: should have been denied!)\n");
    if (fd >= 0) zclose((int)fd);

    long bad = zelevate("wrongpassword");
    zprint("[test_perm] elevate with WRONG password -> ");
    zprint_long(bad);
    zprint(bad < 0 ? " (EXPECTED: rejected)\n" : " (UNEXPECTED: should have been rejected!)\n");

    long uid2 = zgetuid();
    zprint("[test_perm] uid after wrong password: ");
    zprint_long(uid2); zprint("\n");

    long ok = zelevate("wynland");
    zprint("[test_perm] elevate with CORRECT password -> ");
    zprint_long(ok); zprint("\n");

    long uid3 = zgetuid();
    zprint("[test_perm] uid after correct password: ");
    zprint_long(uid3); zprint("\n");

    long fd2 = zopen("/etc/sudopw", O_WRONLY);
    zprint("[test_perm] write-open /etc/sudopw (elevated) -> ");
    zprint_long(fd2);
    zprint(fd2 >= 0 ? " (EXPECTED: allowed)\n" : " (UNEXPECTED: should have been allowed!)\n");
    if (fd2 >= 0) zclose((int)fd2);

    if (uid == 1000 && fd < 0 && bad < 0 && uid2 == 1000 && ok == 0 && uid3 == 0 && fd2 >= 0) {
        zprint("[test_perm] ALL CHECKS PASS\n");
    } else {
        zprint("[test_perm] SOME CHECKS FAILED\n");
    }

    return 0;
}

#include "zerp_entry.h"
