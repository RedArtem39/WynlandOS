/*
 * WynlandOS - real fork() verification.
 * Uses musl's real fork(), pipe(), write(), read() -- not raw syscall
 * wrappers -- to prove the new SYS_fork kernel implementation actually
 * works end-to-end: the child is a genuinely separate, independently-
 * scheduled process that resumes exactly where fork() was called (not a
 * fresh entry point), inherits the pipe fd, and its own memory writes
 * (the "before"/"after" strings) don't corrupt the parent's copy --
 * proving the address-space clone is real and not aliased.
 * Build:
 *   x86_64-linux-musl-gcc -static -O2 -o test_fork.elf test_fork.c
 */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>

int main(void) {
    int pfd[2];
    if (pipe(pfd) != 0) {
        printf("FAIL: pipe() failed\n");
        return 1;
    }

    char shared_buf[32];
    strcpy(shared_buf, "before-fork");

    printf("main: calling fork(), pid before = %d\n", getpid());

    pid_t pid = fork();

    if (pid < 0) {
        printf("FAIL: fork() returned %d\n", (int)pid);
        return 1;
    }

    if (pid == 0) {
        /* Child: mutate its OWN copy of shared_buf, prove the parent's
           copy is untouched (real address-space clone, not aliasing),
           then send a distinct tag through the inherited pipe fd. */
        strcpy(shared_buf, "child-modified");
        write(pfd[1], "CHILD-TAG-OK", 12);
        _exit(0);
    }

    /* Parent: read the child's tag back from the SAME pipe fd it also
       has open (real fd inheritance across fork), and confirm its own
       shared_buf was never touched by the child's write.
       This OS's pipes are non-blocking (documented elsewhere in this
       project) -- a read() on an empty-right-now pipe returns 0
       immediately rather than blocking until the child writes, so poll
       with a bounded retry loop instead of a single read() call. */
    char readback[32] = {0};
    ssize_t n = 0;
    for (int attempt = 0; attempt < 2000000 && n <= 0; attempt++) {
        n = read(pfd[0], readback, sizeof(readback) - 1);
    }

    printf("parent: pid after fork = %d, child pid = %d\n", getpid(), (int)pid);
    printf("parent: shared_buf = '%s' (must still be 'before-fork')\n", shared_buf);
    printf("parent: read %d bytes from pipe: '%s'\n", (int)n, readback);

    if (n == 12 && strcmp(readback, "CHILD-TAG-OK") == 0 &&
        strcmp(shared_buf, "before-fork") == 0) {
        printf("PASS: real fork() works -- separate address space, inherited fd, independent scheduling\n");
        return 0;
    }
    printf("FAIL: fork round-trip mismatch\n");
    return 1;
}
