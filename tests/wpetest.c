/*
 * WynlandOS - WPE WebKit check (glibc, host-built)
 * ============================================================
 * Runs /usr/bin/wpeshot (tests/wpeshot.c) on a few pages, each in its own
 * process tree: a local page (layout, CSS, SVG, canvas, JS through the
 * JIT), a small HTTPS site, YouTube. Every snapshot goes to the serial
 * log as a PNG ("ZSNAP <name> ..."). One "[wpetest] PASS|FAIL" per page.
 *
 * Build: gcc -O2 -o build/wpetest.elf tests/wpetest.c
 */
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_pass, g_fail;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void shot(const char *name, const char *url, const char *settle_ms, const char *timeout_ms)
{
    char *argv[] = { "/usr/bin/wpeshot", (char *)url, (char *)name, (char *)settle_ms, (char *)timeout_ms, NULL };
    char *env[] = {
        "HOME=/tmp", "XDG_CACHE_HOME=/tmp/.cache", "XDG_DATA_HOME=/tmp/.local/share",
        "XDG_RUNTIME_DIR=/tmp", "LD_LIBRARY_PATH=/lib64",
        "WYNLAND_ALLOW_JIT=1",                 /* JavaScriptCore's JIT */
        "WEBKIT_SKIA_ENABLE_CPU_RENDERING=1",  /* first light: no GPU in the web process */
        "GST_DEBUG=1",
        NULL
    };
    pid_t pid;
    long t0 = now_ms();
    int st = 0, ok = 0;
    if (posix_spawn(&pid, argv[0], NULL, NULL, argv, env) == 0 && waitpid(pid, &st, 0) == pid) {
        if (WIFEXITED(st)) ok = WEXITSTATUS(st) == 0;
        else fprintf(stderr, "[wpetest] %s: wpeshot killed by signal %d\n", name, WTERMSIG(st));
    } else {
        fprintf(stderr, "[wpetest] %s: could not run wpeshot\n", name);
    }
    fprintf(stderr, "[wpetest] %s %s (%s) in %ld ms\n", ok ? "PASS" : "FAIL", name, url, now_ms() - t0);
    if (ok) g_pass++; else g_fail++;
}

int main(void)
{
    shot("local", "file:///usr/share/wynland/web/test.html", "1500", "120000");
    shot("example", "https://example.com/", "1500", "180000");
    shot("youtube", "https://www.youtube.com/", "15000", "150000");
    fprintf(stderr, "[wpetest] DONE pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
