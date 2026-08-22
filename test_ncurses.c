/*
 * WynlandOS - non-interactive ncurses smoke test (Phase 18e/18a).
 * Creates a real PTY (kernel primitive from Phase 18a), dup2()s the
 * slave onto its own fd 0/1/2, initializes real ncurses against it,
 * draws something, then reads back the raw VT100 bytes ncurses wrote
 * to the master side -- confirming the whole chain (PTY ring buffers,
 * TCGETS/TCSETS/TIOCGWINSZ ioctls, ncurses' own terminal setup) works
 * before wiring this into zerp_term.c's interactive mode.
 *
 * Build:
 *   x86_64-linux-musl-gcc -O2 -o test_ncurses.elf test_ncurses.c \
 *     -Iexternal_src/ncurses-6.4/build_musl/stage/include \
 *     -Lexternal_src/ncurses-6.4/build_musl/stage/lib -lncurses
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <curses.h>

/* SYS_pty_create isn't in musl's syscall table -- raw syscall, matching
   this project's zerp_syscalls.h convention for custom syscall numbers. */
static int pty_create(int fds[2]) {
    return (int)syscall(410, fds);
}

int main(void) {
    int fds[2];
    if (pty_create(fds) != 0) {
        printf("FAIL: pty_create() failed\n");
        return 1;
    }
    int master = fds[0], slave = fds[1];
    printf("test_ncurses: pty_create() ok, master=%d slave=%d\n", master, slave);

    /* Give ncurses a real 24x80 to work with via TIOCSWINSZ. */
    struct winsize ws = { 24, 80, 0, 0 };
    ioctl(slave, TIOCSWINSZ, &ws);

    /* Redirect our own stdio onto the slave -- ncurses' initscr() reads
       TERM and does I/O through stdin/stdout by default. */
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);

    setenv("TERM", "vt100", 1);

    WINDOW *w = initscr();
    if (!w) {
        write(2, "FAIL: initscr() returned NULL\n", 31);
        return 1;
    }
    printw("Hello from ncurses on WynlandOS!");
    move(1, 0);
    printw("Line two.");
    refresh();
    endwin();

    /* Read back whatever ncurses actually wrote to the slave, via the
       master side -- this is the raw VT100 byte stream a real terminal
       emulator (zerp_term.c, Phase 18g/h) would need to interpret. */
    char buf[4096];
    int total = 0;
    for (int i = 0; i < 20 && total < (int)sizeof(buf) - 1; i++) {
        int n = (int)read(master, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = 0;

    /* Can't use fd 1/2 anymore (redirected to the now-closed curses
       session) -- write results directly to fd 3 (serial-visible via
       the kernel's raw console path is fd 1/2 only, so reopen a path
       that still reaches the log: fd 1 was dup'd to the pty slave, but
       the kernel's console/serial special-case only fires when
       fd_table[1] is NULL. Simplest: write the summary to the ORIGINAL
       console before it got redirected is no longer possible, so print
       via a fresh raw write() to fd 2 after re-pointing it back). */
    close(1); close(2);
    /* fd 1/2 are now free again (NULL in fd_table) so the kernel's
       raw console-write shortcut applies once more. */
    char summary[256];
    int slen = 0;
    const char *p1 = "test_ncurses: read back ";
    while (*p1) summary[slen++] = *p1++;
    /* simple itoa */
    char numbuf[16]; int ni = 0; int tv = total;
    if (tv == 0) numbuf[ni++] = '0';
    while (tv > 0) { numbuf[ni++] = '0' + (tv % 10); tv /= 10; }
    while (ni > 0) summary[slen++] = numbuf[--ni];
    const char *p2 = " bytes from master\n";
    while (*p2) summary[slen++] = *p2++;
    write(1, summary, slen);

    write(1, "test_ncurses: first 80 raw bytes (escape sequences expected) = [", 66);
    int show = total < 80 ? total : 80;
    write(1, buf, show);
    write(1, "]\n", 2);

    int has_esc = 0;
    for (int i = 0; i < total; i++) if (buf[i] == 0x1B) { has_esc = 1; break; }

    if (total > 0 && has_esc) {
        write(1, "PASS: real ncurses initscr/printw/refresh/endwin produced real VT100 escape sequences on WynlandOS\n", 102);
        return 0;
    }
    write(1, "FAIL: no escape sequences found in ncurses output\n", 51);
    return 1;
}
