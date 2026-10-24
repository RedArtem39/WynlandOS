/*
 * WynlandOS - gpuclock: which of the GPU clock (RPS) registers stops the
 * laptop dead (drivers/gpu/i915.c gt_clock_up hung it before its first
 * line). One register a step, each step announced over UDP to the
 * developer's machine first (tools/devhost.py writes it to the boot log),
 * so the last line that arrives names the culprit.
 *
 *   gpuclock          reads only
 *   gpuclock set      + the frequency request (RPNSWREQ) at RP0
 *   gpuclock control  + RP_CONTROL in software mode first, as the kernel did
 *   gpuclock step     the request raised 50 MHz a step from where it is, each
 *                     step announced first (where the machine stops, if it does)
 *
 * Forcewake with the "user" bit (1): the kernel's driver uses bit 0.
 * gcc -O2 -o gpuclock gpuclock.c
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "gpulib.h"

static int g_sock = -1;
static struct sockaddr_in g_to;

static void step(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 3) n = sizeof line - 3;
    line[n++] = '\n';
    line[n] = 0;
    fputs(line, stdout);
    fflush(stdout);
    if (g_sock >= 0) sendto(g_sock, line, (size_t)n, 0, (struct sockaddr *)&g_to, sizeof g_to);
    usleep(20000);                                    /* (the packet out before the next step) */
}

static uint32_t rd_step(const char *name, uint32_t reg)
{
    step("gpuclock: reading %s (0x%x) ...", name, reg);
    uint32_t v = rd(reg);
    step("gpuclock:   %s = 0x%08x", name, v);
    return v;
}

static int user_wake(uint32_t req, uint32_t ack, int on)
{
    wr(req, masked(2, on));
    uint64_t t0 = now_us();
    while ((((rd(ack) >> 1) & 1) != (uint32_t)on) && now_us() - t0 < 50000) ;
    return ((rd(ack) >> 1) & 1) == (uint32_t)on;
}

int main(int argc, char **argv)
{
    int set = argc > 1 && (!strcmp(argv[1], "set") || !strcmp(argv[1], "control"));
    int control = argc > 1 && !strcmp(argv[1], "control");
    int stepping = argc > 1 && !strcmp(argv[1], "step");
    /* the developer's machine: boot.cfg's netlog= */
    char cfg[1024] = { 0 }, host[64] = "192.168.0.239";
    FILE *f = fopen("/etc/wynland/boot.cfg", "r");
    if (f) { size_t n = fread(cfg, 1, sizeof cfg - 1, f); cfg[n] = 0; fclose(f); }
    char *p = strstr(cfg, "netlog=");
    if (p) { p += 7; int i = 0; while (*p && *p != ':' && i < 63) host[i++] = *p++; host[i] = 0; }
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&g_to, 0, sizeof g_to);
    g_to.sin_family = AF_INET;
    g_to.sin_port = htons(5140);
    inet_pton(AF_INET, host, &g_to.sin_addr);

    step("gpuclock: start (%s), the log to %s", control ? "control" : set ? "set" : "read only", host);
    gpu_open();
    step("gpuclock: forcewake (user bit) ...");
    int ok = user_wake(0xA188, 0x130044, 1) && user_wake(0xA278, 0x0D84, 1);
    step("gpuclock:   forcewake %s", ok ? "ok" : "NO ACK");
    uint32_t cap = rd_step("RP_STATE_CAP", 0x145998);
    uint32_t rp0 = cap & 0xFF, rpn = (cap >> 16) & 0xFF;
    step("gpuclock:   RP0 %u MHz, RP1 %u MHz, RPn %u MHz", rp0 * 50, ((cap >> 8) & 0xFF) * 50, rpn * 50);
    rd_step("RPSTAT1 (Gen9 place)", 0xA01C);
    uint32_t st = rd_step("GEN12_RPSTAT1", 0x1381B4);
    step("gpuclock:   now %u MHz (GEN12_RPSTAT1 bits 19:11)", ((st >> 11) & 0x1FF) * 50 / 3);
    rd_step("RP_CONTROL", 0xA024);
    rd_step("RPNSWREQ", 0xA008);
    rd_step("RP_INTERRUPT_LIMITS", 0xA014);
    if (stepping && rp0) {
        uint32_t cur = rd(0xA008) >> 23;              /* 16.67 MHz units */
        for (uint32_t f2 = cur + 3; f2 <= rp0 * 3; f2 += 3) {
            step("gpuclock: step: writing RPNSWREQ = %u MHz ...", f2 * 50 / 3);
            wr(0xA008, f2 << 23);
            step("gpuclock:   written; 50 ms");
            usleep(50000);
            st = rd_step("GEN12_RPSTAT1", 0x1381B4);
            step("gpuclock:   now %u MHz", ((st >> 11) & 0x1FF) * 50 / 3);
        }
    }
    if (set && rp0) {
        if (control) {
            step("gpuclock: writing RP_CONTROL = media is gfx | enable (software mode) ...");
            wr(0xA024, (1u << 8) | (1u << 7));
            step("gpuclock:   written");
        }
        step("gpuclock: writing RPNSWREQ = RP0 (%u MHz) ...", rp0 * 50);
        wr(0xA008, (rp0 * 3) << 23);
        step("gpuclock:   written; waiting 100 ms");
        usleep(100000);
        st = rd_step("GEN12_RPSTAT1", 0x1381B4);
        step("gpuclock:   now %u MHz", ((st >> 11) & 0x1FF) * 50 / 3);
    }
    user_wake(0xA278, 0x0D84, 0);
    user_wake(0xA188, 0x130044, 0);
    step("gpuclock: done");
    return 0;
}
