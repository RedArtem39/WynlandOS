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

/* action/status: JavaScript run once the page loaded / printed before the
   snapshot (a status starting with "FAIL" fails the page) */
/* boot.cfg "wpeonly=a,b": only those pages (quicker debugging runs) */
static char g_only[256];
static int wanted(const char *name)
{
    if (!g_only[0]) return 1;
    size_t n = strlen(name);
    for (const char *p = g_only; (p = strstr(p, name)); p += n)
        if ((p == g_only || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return 1;
    return 0;
}

static void shot_js(const char *name, const char *url, const char *settle_ms, const char *timeout_ms,
                    const char *action, const char *status)
{
    if (!wanted(name)) return;
    char *argv[] = { "/usr/bin/wpeshot", (char *)url, (char *)name, (char *)settle_ms, (char *)timeout_ms,
                     (char *)(action ? action : ""), (char *)(status ? status : ""), NULL };
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

static void shot(const char *name, const char *url, const char *settle_ms, const char *timeout_ms)
{
    shot_js(name, url, settle_ms, timeout_ms, NULL, NULL);
}

/* play the page's <video>; then: is it playing, and what did it decode? */
static const char PLAY_JS[] =
    "(() => { const v = document.querySelector('video'); if (!v) return 'no video';"
    " const p = v.play(); if (p) p.catch(e => console.log('play(): ' + e)); return 'play() called'; })()";
static const char VIDEO_STATUS_JS[] =
    "(() => { const v = document.querySelector('video'); if (!v) return 'FAIL no <video>';"
    " const q = v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : null;"
    " return (v.currentTime > 2 && !v.paused ? 'PLAYING' : 'FAIL')"
    " + ' t=' + v.currentTime.toFixed(1) + ' paused=' + v.paused + ' ready=' + v.readyState"
    " + ' err=' + (v.error ? v.error.code + ':' + v.error.message : 'none')"
    " + ' frames=' + (q ? q.totalVideoFrames + ' dropped=' + q.droppedVideoFrames : '?')"
    " + ' ' + v.videoWidth + 'x' + v.videoHeight; })()";

/* the network's speed, for the page timings below: curl downloads 1.25 MB
   over HTTPS (bytes, seconds, bytes/s on stdout) */
static void netspeed(void)
{
    int fd[2];
    if (pipe(fd) != 0) return;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    char *argv[] = { "/usr/bin/curl", "-s", "-o", "/dev/null", "--max-time", "120",
                     "-w", "%{size_download} bytes in %{time_total} s (%{speed_download} B/s), first byte %{time_starttransfer} s",
                     "https://proof.ovh.net/files/10Mb.dat", NULL };
    char *env[] = { "CURL_CA_BUNDLE=/etc/ssl/cert.pem", NULL };
    pid_t pid;
    char out[256] = "";
    if (posix_spawn(&pid, argv[0], &fa, NULL, argv, env) == 0) {
        close(fd[1]);
        ssize_t n = read(fd[0], out, sizeof out - 1);
        if (n > 0) out[n] = 0;
        waitpid(pid, NULL, 0);
    } else {
        close(fd[1]);
    }
    close(fd[0]);
    posix_spawn_file_actions_destroy(&fa);
    fprintf(stderr, "[wpetest] network: %s\n", out[0] ? out : "curl failed");
}

int main(void)
{
    FILE *cfg = fopen("/etc/wynland/boot.cfg", "r");
    char line[256];
    while (cfg && fgets(line, sizeof line, cfg))
        if (strncmp(line, "wpeonly=", 8) == 0) {
            strncpy(g_only, line + 8, sizeof g_only - 1);
            g_only[strcspn(g_only, "\r\n")] = 0;
        }
    if (cfg) fclose(cfg);
    if (wanted("net")) netspeed();
    /* JavaScript speed: 10 million loop steps (JIT: tens of ms; the
       interpreter alone: about a second) */
    shot_js("local", "file:///usr/share/wynland/web/test.html", "1500", "120000", NULL,
            "(() => { const t = performance.now(); let s = 0; for (let i = 0; i < 1e7; i++) s += i % 7;"
            " return 'js 1e7 loop: ' + Math.round(performance.now() - t) + ' ms (' + s + ')'; })()");
    shot("example", "https://example.com/", "1500", "180000");
    /* how far YouTube's own rendering got: animation frames ticking, icons drawn */
    shot_js("youtube", "https://www.youtube.com/", "15000", "150000",
            "window.__raf = 0; window.__tick = 0; window.__t0 = performance.now();"
            " (function f() { window.__raf++; requestAnimationFrame(f); })();"
            " setInterval(() => window.__tick++, 10); 'counting frames and timer ticks'",
            "'in ' + Math.round(performance.now() - window.__t0) + ' ms: raf=' + window.__raf"
            " + ' timer10ms=' + window.__tick + ' yt-icon=' + document.querySelectorAll('yt-icon').length"
            " + ' with-svg=' + document.querySelectorAll('yt-icon svg').length"
            " + ' guide=' + !!document.querySelector('ytd-mini-guide-renderer')"
            " + ' logo=' + document.querySelectorAll('#logo-icon svg').length"
            /* where the load time went: the page itself and the slowest
               resources -- dns / connect (incl. TLS) / wait for the first
               byte / transfer, in ms, and their size */
            " + (() => { const r = (e) => Math.round(e); const ph = (e) => (e.name.split('?')[0].slice(-48))"
            " + ' start=' + r(e.startTime) + ' dns=' + r(e.domainLookupEnd - e.domainLookupStart)"
            " + ' conn=' + r(e.connectEnd - e.connectStart) + ' ttfb=' + r(e.responseStart - e.requestStart)"
            " + ' xfer=' + r(e.responseEnd - e.responseStart) + ' kb=' + r((e.encodedBodySize || 0) / 1024);"
            " const nav = performance.getEntriesByType('navigation')[0];"
            " const res = performance.getEntriesByType('resource');"
            " const slow = res.slice().sort((a, b) => b.duration - a.duration).slice(0, 8);"
            " return ' | page: ' + (nav ? ph(nav) + ' domready=' + r(nav.domContentLoadedEventEnd) : '?')"
            " + ' | ' + res.length + ' resources, ' + r(res.reduce((s, e) => s + (e.encodedBodySize || 0), 0) / 1024) + ' kb'"
            " + ' | slowest: ' + slow.map(e => r(e.duration) + 'ms ' + ph(e)).join(' ; '); })()");
    /* a video: Big Buck Bunny (Blender Foundation, CC BY), no DRM */
    shot_js("ytwatch", "https://www.youtube.com/watch?v=aqz-KE-bpKQ", "30000", "300000",
            PLAY_JS, VIDEO_STATUS_JS);
    fprintf(stderr, "[wpetest] DONE pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
