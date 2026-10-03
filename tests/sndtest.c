/*
 * WynlandOS - sound check (glibc, host-built)
 * ============================================================
 * Opens /dev/dsp the way GStreamer's osssink does (OSS ioctls), plays
 * 1.5 s of a 440 Hz sine, and checks the card really consumes it: the
 * queued amount (GETODELAY) must drain. One "[sndtest] PASS|FAIL" line
 * per check.
 *
 * Build: gcc -O2 -o build/sndtest.elf tests/sndtest.c -lm
 */
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define SNDCTL_DSP_RESET     0x00005000
#define SNDCTL_DSP_SYNC      0x00005001
#define SNDCTL_DSP_SPEED     0xC0045002
#define SNDCTL_DSP_SETFMT    0xC0045005
#define SNDCTL_DSP_CHANNELS  0xC0045006
#define SNDCTL_DSP_GETOSPACE 0x8010500C
#define SNDCTL_DSP_GETODELAY 0x80045017
#define AFMT_S16_LE          0x10

static int g_pass, g_fail;
static void check(int ok, const char *what)
{
    fprintf(stderr, "[sndtest] %s %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) g_pass++; else g_fail++;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void)
{
    int fd = open("/dev/dsp", O_WRONLY);
    check(fd >= 0, "open /dev/dsp");
    if (fd < 0) { fprintf(stderr, "[sndtest] DONE pass=%d fail=%d\n", g_pass, g_fail); return 1; }

    int fmt = AFMT_S16_LE, ch = 2, rate = 48000;
    check(ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) == 0 && fmt == AFMT_S16_LE, "SETFMT S16_LE");
    check(ioctl(fd, SNDCTL_DSP_CHANNELS, &ch) == 0 && ch == 2, "CHANNELS 2");
    check(ioctl(fd, SNDCTL_DSP_SPEED, &rate) == 0 && rate == 48000, "SPEED 48000");
    /* after a RESET a short write is all still queued (the lead-in
       silence was subtracted from it) */
    {
        static short shortbuf[1024 * 2];   /* 4 KB of silence */
        int q = 0;
        struct timespec t0, t1;
        ioctl(fd, SNDCTL_DSP_RESET, 0);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        write(fd, shortbuf, sizeof shortbuf);
        ioctl(fd, SNDCTL_DSP_GETODELAY, &q);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        /* minus what the card played meanwhile (48 kHz stereo 16-bit: 192
           bytes a millisecond) -- other autotests run alongside and this
           process can be preempted between the two calls */
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        int played = (int)(ms + 1) * 192;
        check(q > 0 && q >= (int)sizeof shortbuf - played, "GETODELAY counts a fresh write");
        ioctl(fd, SNDCTL_DSP_SYNC, 0);
    }
    int space[4] = {0};
    check(ioctl(fd, SNDCTL_DSP_GETOSPACE, space) == 0 && space[3] > 0, "GETOSPACE");

    /* 1.5 s of 440 Hz, -12 dB */
    static short buf[4800 * 2];   /* 100 ms */
    long t0 = now_ms();
    double ph = 0;
    for (int block = 0; block < 15; block++) {
        for (int i = 0; i < 4800; i++) {
            short v = (short)(8000 * sin(ph));
            ph += 2 * M_PI * 440.0 / 48000.0;
            /* left: a sample counter (a ramp), so a recording shows every
               dropped or repeated sample; right: the tone */
            buf[2 * i] = (short)(((block * 4800 + i) % 20000) - 10000);
            buf[2 * i + 1] = v;
        }
        if (write(fd, buf, sizeof buf) != (ssize_t)sizeof buf) { check(0, "write"); break; }
    }
    long t1 = now_ms();
    int d1 = 0, d2 = 0;
    ioctl(fd, SNDCTL_DSP_GETODELAY, &d1);
    usleep(150 * 1000);
    ioctl(fd, SNDCTL_DSP_GETODELAY, &d2);
    fprintf(stderr, "[sndtest] wrote 1.5 s in %ld ms; queued %d -> %d bytes over 150 ms\n", t1 - t0, d1, d2);
    /* at most ~0.5 s can be queued: writing 1.5 s takes ~1 s of playback */
    check(t1 - t0 > 700, "writes are paced by playback");
    check(d1 > 0 && d2 < d1, "queued audio drains");
    check(ioctl(fd, SNDCTL_DSP_SYNC, 0) == 0, "SYNC");
    ioctl(fd, SNDCTL_DSP_GETODELAY, &d2);
    check(d2 == 0, "nothing left after SYNC");
    close(fd);

    fprintf(stderr, "[sndtest] DONE pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
