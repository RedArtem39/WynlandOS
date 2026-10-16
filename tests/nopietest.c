/* A program linked at a fixed address (not PIE: Ubuntu's gcc and many
   older binaries are), run from /bin/sh by tests/shtest.c, dynamic and
   static. Its code sits at 0x400000, in the lower half that the kernel no
   longer shares with programs (include/wynland/memlayout.h). Prints
   "nopie ok" when its code is where it was linked, malloc works (brk for
   the static one), and a kernel address is not readable (SIGSEGV). */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    uintptr_t code = (uintptr_t)&main;
    if (code < 0x400000 || code >= 0x1000000) {
        printf("nopie: main at %#lx, not where it was linked\n", (unsigned long)code);
        return 1;
    }
    char *p = malloc(1 << 20);
    if (!p) { printf("nopie: malloc failed\n"); return 1; }
    memset(p, 0x5a, 1 << 20);
    if (p[12345] != 0x5a) { printf("nopie: malloc'd memory broken\n"); return 1; }
    free(p);

    /* the kernel's image: mapped, but not for user code -- a child that
       reads it dies of SIGSEGV */
    fflush(stdout);
    pid_t c = fork();
    if (c == 0) {
        volatile unsigned char v = *(volatile unsigned char *)0xFFFFFFFF80100000ULL;
        (void)v;
        _exit(0);
    }
    int st = 0;
    waitpid(c, &st, 0);
    if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGSEGV) {
        printf("nopie: read the kernel's memory\n");
        return 1;
    }
    printf("nopie ok\n");
    return 0;
}
