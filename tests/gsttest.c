/*
 * WynlandOS - GStreamer check (glibc, host-built)
 * ============================================================
 * Runs Ubuntu's gst-inspect-1.0 / gst-launch-1.0 on WynlandOS:
 *   - the plugin registry builds and osssink is there,
 *   - a Vorbis tone plays through osssink -> /dev/dsp in real time,
 *   - the H.264 + AAC MP4 clip demuxes and decodes (gst-libav), its
 *     audio to osssink, its video to a fakesink.
 * One "[gsttest] PASS|FAIL" line per check.
 *
 * Build: gcc -O2 -o build/gsttest.elf tests/gsttest.c
 */
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static int g_pass, g_fail;
static void check(int ok, const char *what)
{
    fprintf(stderr, "[gsttest] %s %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) g_pass++; else g_fail++;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
/* run argv, return its exit status (-1: did not run), *ms = wall time */
static int run(char *const argv[], long *ms)
{
    char *env[] = {
        "HOME=/tmp", "XDG_CACHE_HOME=/tmp/.cache", "LD_LIBRARY_PATH=/lib64",
        "GST_DEBUG=2", "GST_DEBUG_NO_COLOR=1", NULL
    };
    pid_t pid;
    long t0 = now_ms();
    if (posix_spawn(&pid, argv[0], NULL, NULL, argv, env) != 0) return -1;
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    if (ms) *ms = now_ms() - t0;
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    fprintf(stderr, "[gsttest] %s killed by signal %d\n", argv[0], WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    return -1;
}

#define REGISTRY "/tmp/.cache/gstreamer-1.0/registry.x86_64.bin"

/* what GStreamer does to save its registry: mkdir -p, mkstemp, fdopen
   (which checks the fd's access mode), write, fsync, close, rename over
   the old one. And what filesrc does: lseek must return the new offset. */
static int registry_like_write(void)
{
    mkdir("/tmp/.cache", 0700);
    if (mkdir("/tmp/.cache/gstreamer-1.0", 0700) < 0 && errno != EEXIST) {
        fprintf(stderr, "[gsttest] mkdir: errno %d\n", errno); return 0;
    }
    char tmp[] = "/tmp/.cache/gstreamer-1.0/probe.bin.XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) { fprintf(stderr, "[gsttest] mkstemp: errno %d\n", errno); return 0; }
    FILE *f = fdopen(fd, "wb");
    if (!f) { fprintf(stderr, "[gsttest] fdopen: errno %d\n", errno); return 0; }
    static char buf[200000];
    memset(buf, 0x5a, sizeof buf);
    if (fwrite(buf, 1, sizeof buf, f) != sizeof buf || fflush(f) != 0) {
        fprintf(stderr, "[gsttest] write: errno %d\n", errno); return 0;
    }
    off_t at = lseek(fd, 1000, SEEK_SET), end = lseek(fd, 0, SEEK_END);
    if (at != 1000 || end != (off_t)sizeof buf) {
        fprintf(stderr, "[gsttest] lseek: %ld %ld\n", (long)at, (long)end); return 0;
    }
    if (fsync(fd) < 0) { fprintf(stderr, "[gsttest] fsync: errno %d\n", errno); return 0; }
    if (fclose(f) != 0) { fprintf(stderr, "[gsttest] close: errno %d\n", errno); return 0; }
    if (rename(tmp, "/tmp/.cache/gstreamer-1.0/probe.bin") < 0) {
        fprintf(stderr, "[gsttest] rename: errno %d\n", errno); return 0;
    }
    unlink("/tmp/.cache/gstreamer-1.0/probe.bin");
    return 1;
}

int main(void)
{
    long ms = 0;

    check(registry_like_write(), "mkstemp + fdopen + write + lseek + fsync + rename (how the registry is saved)");
    check(access("/lib64/../../lib/x86_64-linux-gnu/gstreamer1.0/gstreamer-1.0/gst-plugin-scanner", X_OK) == 0,
          "plugin scanner where libgstreamer looks for it");

    char *inspect[] = { "/usr/bin/gst-inspect-1.0", "osssink", NULL };
    int r = run(inspect, &ms);
    fprintf(stderr, "[gsttest] gst-inspect-1.0 osssink: rc=%d, %ld ms (first run builds the registry)\n", r, ms);
    check(r == 0, "plugin registry has osssink");
    check(access(REGISTRY, R_OK) == 0, "registry cache saved");

    char *libav[] = { "/usr/bin/gst-inspect-1.0", "avdec_h264", NULL };
    r = run(libav, &ms);
    fprintf(stderr, "[gsttest] gst-inspect-1.0 avdec_h264: rc=%d, %ld ms (from the cache)\n", r, ms);
    check(r == 0, "gst-libav has avdec_h264");

    /* ~3 s of 440 Hz */
    char *tone[] = { "/usr/bin/gst-launch-1.0", "-q",
        "filesrc", "location=/usr/share/wynland/media/tone.ogg", "!", "oggdemux", "!", "vorbisdec",
        "!", "audioconvert", "!", "audioresample", "!", "osssink", "device=/dev/dsp", NULL };
    r = run(tone, &ms);
    fprintf(stderr, "[gsttest] tone.ogg: rc=%d, %ld ms\n", r, ms);
    check(r == 0, "Ogg Vorbis plays through osssink");
    check(r == 0 && ms > 2000, "...in real time (paced by the sound card)");

    /* ~3 s of H.264 video + AAC audio (880 Hz) */
    char *clip[] = { "/usr/bin/gst-launch-1.0", "-q",
        "filesrc", "location=/usr/share/wynland/media/clip.mp4", "!", "decodebin", "name=d",
        "d.", "!", "queue", "!", "videoconvert", "!", "fakesink", "sync=true",
        "d.", "!", "queue", "!", "audioconvert", "!", "audioresample", "!", "osssink", "device=/dev/dsp", NULL };
    r = run(clip, &ms);
    fprintf(stderr, "[gsttest] clip.mp4: rc=%d, %ld ms\n", r, ms);
    check(r == 0, "H.264 + AAC MP4 decodes (decodebin, gst-libav)");
    check(r == 0 && ms > 2000, "...and plays in real time");

    fprintf(stderr, "[gsttest] DONE pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
