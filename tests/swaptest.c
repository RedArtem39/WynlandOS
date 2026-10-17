/*
 * WynlandOS - swap check (glibc, host-built)
 * ============================================================
 * Fills more memory than the machine has free, so pages must go out to
 * the swap partition and come back (kernel/swap.c): every page carries its
 * own number, written once and checked after. Then a forked child checks
 * the same memory (its pages in swap are shared slots until read), writes
 * some of it, and the parent checks its own copy is untouched. Unmapping
 * gives the swap space back. One "[swaptest] PASS|FAIL ..." line per check.
 * Boot with AUTOTEST=swap and a small MEM (make run-gl MEM=768M).
 *
 * Build: gcc -O2 -o build/swaptest.elf tests/swaptest.c
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    fprintf(stderr, "[swaptest] %s %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) g_pass++; else g_fail++;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

#define PG 4096UL

/* page i holds i (and its complement at the end), salted per writer */
static void fill(uint8_t *m, size_t pages, uint64_t salt)
{
    for (size_t i = 0; i < pages; i++) {
        uint64_t *w = (uint64_t *)(m + i * PG);
        w[0] = i ^ salt;
        w[PG / 8 - 1] = ~(i ^ salt);
    }
}

static size_t bad_pages(const uint8_t *m, size_t pages, uint64_t salt, size_t step)
{
    size_t bad = 0;
    for (size_t i = 0; i < pages; i += step) {
        const uint64_t *w = (const uint64_t *)(m + i * PG);
        if (w[0] != (i ^ salt) || w[PG / 8 - 1] != ~(i ^ salt)) bad++;
    }
    return bad;
}

int main(void)
{
    struct sysinfo si;
    sysinfo(&si);
    fprintf(stderr, "[swaptest] RAM %lu MB, %lu MB free, swap %lu MB (%lu MB free)\n",
            si.totalram >> 20, si.freeram >> 20, si.totalswap >> 20, si.freeswap >> 20);
    check(si.totalswap >= (64UL << 20), "a swap partition is there");
    if (si.totalswap < (64UL << 20)) {
        fprintf(stderr, "[swaptest] DONE pass=%d fail=%d\n", g_pass, g_fail);
        return 1;
    }
    uint64_t swap_free0 = si.freeswap;

    /* what is free now, plus a good part of the swap */
    /* a quarter of the swap at most: the forked child below needs room
       for its own copies too (more is a real out-of-memory) */
    size_t extra = si.freeswap / 4 < (128UL << 20) ? si.freeswap / 4 : (128UL << 20);
    size_t bytes = si.freeram + extra;
    size_t pages = bytes / PG;
    uint8_t *m = mmap(NULL, pages * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(m != MAP_FAILED, "a mapping bigger than the free RAM");
    if (m == MAP_FAILED) return 1;

    long t0 = now_ms();
    fill(m, pages, 0);
    long t1 = now_ms();
    sysinfo(&si);
    fprintf(stderr, "[swaptest] wrote %zu MB in %ld ms; %lu MB free, %lu MB in swap\n",
            (pages * PG) >> 20, t1 - t0, si.freeram >> 20, (si.totalswap - si.freeswap) >> 20);
    check(si.totalswap - si.freeswap > (16UL << 20), "pages went out to swap");

    size_t bad = bad_pages(m, pages, 0, 1);
    long t2 = now_ms();
    fprintf(stderr, "[swaptest] read it all back in %ld ms: %zu bad pages\n", t2 - t1, bad);
    check(bad == 0, "every page reads back what was written");

    /* a child: the same contents (swap slots shared), then its own */
    pid_t c = fork();
    if (c == 0) {
        size_t cb = bad_pages(m, pages, 0, 16);
        fill(m, pages / 16, 0x5A5A);                      /* the child writes a sixteenth */
        size_t cb2 = bad_pages(m, pages / 16, 0x5A5A, 1);
        _exit(cb || cb2 ? 1 : 0);
    }
    int st = 0;
    waitpid(c, &st, 0);
    check(c > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "a forked child sees the same memory, then its own");
    check(bad_pages(m, pages, 0, 1) == 0, "the parent's memory is untouched by the child's writes");

    sysinfo(&si);
    uint64_t used_before = si.totalswap - si.freeswap;
    munmap(m, pages * PG);
    /* this mapping's slots come back; other programs' pages pushed out
       meanwhile may stay there (they are theirs) */
    sysinfo(&si);
    uint64_t used_after = si.totalswap - si.freeswap;
    fprintf(stderr, "[swaptest] swap used: %lu MB before munmap, %lu MB after (%lu MB at the start)\n",
            used_before >> 20, used_after >> 20, (si.totalswap - swap_free0) >> 20);
    check(used_after < used_before / 4 + (8UL << 20), "unmapping gives the swap space back");

    fprintf(stderr, "[swaptest] DONE pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
