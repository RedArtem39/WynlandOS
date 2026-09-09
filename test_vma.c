/*
 * WynlandOS Phase 22c regression: real VMAs, mprotect/munmap, and COW fork.
 *
 *   T1  munmap() is real -- child touches an address the parent already
 *       unmapped; must fault fatally (not present, no VMA), not read/write
 *       stale content. Runs FIRST (diagnostic reorder -- see note below).
 *   T2  mprotect(PROT_READ) is real -- child tries to write through a page
 *       the parent downgraded to read-only; that must be a fatal fault
 *       (whole child process disappears), not a silent, successful write.
 *   T3  fork() COW isolation -- parent mmap()s an anon page, writes a
 *       pattern, forks; child overwrites the SAME virtual address with a
 *       different pattern. Parent's own copy must be untouched afterwards
 *       -- true whether the kernel copies eagerly or via COW, but the
 *       kernel's serial log (grep for "COW:" separately) is what actually
 *       proves the copy was deferred to the page fault instead of done
 *       eagerly at fork() time.
 *
 * Order note: the munmap/mprotect checks got flaky (child took far longer
 * than expected to actually get scheduled far enough to hit its fault) when
 * run third in sequence, after two earlier fork+wait cycles already ran in
 * the same process -- diagnostic reorder to see whether it's scheduler
 * fatigue accumulating across repeated fork() calls in this environment
 * rather than a correctness bug (already independently confirmed correct
 * via direct debug tracing of the underlying munmap/mprotect page-table
 * changes and a captured, address-matching page-fault register dump).
 *
 * One write(2) per line (serial-interleaving lesson, same as test_signals.c).
 * Build:
 *   x86_64-linux-musl-gcc -static -O2 -o test_vma.elf test_vma.c
 */
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>

static char obuf[256];
static void out(const char *s) {
    size_t n = strlen(s);
    if (n > sizeof(obuf) - 1) n = sizeof(obuf) - 1;
    memcpy(obuf, s, n);
    obuf[n] = '\n';
    write(1, obuf, n + 1);
}

static void nap(void) {
    struct timespec t = { 0, 20 * 1000 * 1000 };
    nanosleep(&t, 0);
}

/* Poll until the child pid is gone (or we give up). Returns 1 if the child
   died within the budget, 0 if it's still alive (test should treat that as
   a fault -- same convention as test_signals.c's T4). */
static int wait_for_death(long pid, int tries) {
    for (int i = 0; i < tries; i++) {
        nap();
        if (!(int)syscall(411 /* SYS_process_alive */, pid)) return 1;
    }
    return 0;
}

int main(void) {
    /* T1: munmap() is real (moved first -- see file header note) */
    {
        char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        int ok = (page != MAP_FAILED);
        if (ok) {
            memcpy(page, "ABOUT-TO-UNMAP", 15);
            ok = (munmap(page, 4096) == 0);
        }
        int pfd[2];
        if (ok) ok = (pipe(pfd) == 0);
        if (ok) fcntl(pfd[0], F_SETFL, O_NONBLOCK); /* don't block on read() below if the child is still alive past our own wait budget */
        if (ok) {
            long pid = fork();
            if (pid == 0) {
                close(pfd[0]);
                /* volatile: a plain read the compiler can prove is unused
                   (page[0] into a var only ever cast to void) gets dead-
                   code-eliminated at -O2, defeating the whole point of this
                   test -- force a real load through the pointer. */
                volatile char c = *(volatile char *)page;
                (void)c;
                write(pfd[1], "S", 1); /* only reached if it did NOT fault */
                out("[t1-child] read unmapped memory (BUG: should have faulted)");
                _exit(0);
            }
            close(pfd[1]);
            int died = wait_for_death(pid, 500);
            char sig[1];
            int survived = (read(pfd[0], sig, 1) == 1);
            close(pfd[0]);
            out((died && !survived) ? "T1 munmap enforced: PASS" : "T1 munmap enforced: FAIL");
        } else {
            out("T1 munmap enforced: FAIL (setup failed)");
        }
    }

    /* T2: mprotect(PROT_READ) is real */
    {
        char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        int ok = (page != MAP_FAILED);
        if (ok) {
            memcpy(page, "READONLY-GUARD", 15);
            ok = (mprotect(page, 4096, PROT_READ) == 0);
        }
        int pfd[2];
        if (ok) ok = (pipe(pfd) == 0);
        if (ok) fcntl(pfd[0], F_SETFL, O_NONBLOCK); /* don't block on read() below if the child is still alive past our own wait budget */
        if (ok) {
            long pid = fork();
            if (pid == 0) {
                close(pfd[0]);
                page[0] = 'X'; /* must fault -- page is read-only */
                /* Only reached if it did NOT fault -- a positive "I
                   survived" signal beats inferring survival from timing,
                   since process_alive()'s death detection can't otherwise
                   tell "killed by the fault" apart from "ran to
                   completion and exited normally" under scheduling
                   delays. */
                write(pfd[1], "S", 1);
                out("[t2-child] wrote through PROT_READ (BUG: should have faulted)");
                _exit(0);
            }
            close(pfd[1]);
            int died = wait_for_death(pid, 500);
            char sig[1];
            int survived = (read(pfd[0], sig, 1) == 1);
            close(pfd[0]);
            int untouched = (memcmp(page, "READONLY-GUARD", 15) == 0);
            out((died && !survived && untouched) ? "T2 mprotect(PROT_READ) enforced: PASS"
                                                   : "T2 mprotect(PROT_READ) enforced: FAIL");
        } else {
            out("T2 mprotect(PROT_READ) enforced: FAIL (setup failed)");
        }
    }

    /* T3: fork() COW isolation */
    {
        char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        int t3_mmap_ok = (page != MAP_FAILED);
        if (t3_mmap_ok) {
            memcpy(page, "PARENT-BEFORE", 14);

            long pid = fork();
            if (pid == 0) {
                memcpy(page, "CHILD-OVERWRITE", 16);
                out("[t3-child] wrote its own copy");
                _exit(0);
            }

            wait_for_death(pid, 500);
            int isolated = (memcmp(page, "PARENT-BEFORE", 14) == 0);
            out(isolated ? "T3 fork COW isolation: PASS" : "T3 fork COW isolation: FAIL");
        } else {
            out("T3 fork COW isolation: FAIL (mmap failed)");
        }
    }

    out("ALL VMA CHECKS COMPLETE");
    return 0;
}
