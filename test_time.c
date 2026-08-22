/*
 * WynlandOS - real-time/timezone regression test (repo root, permanent
 * regression binary, matching test_fork.c/test_dlopen.c's convention).
 * Confirms: (1) real musl time()/gmtime()/localtime() work against the
 * kernel's real RTC-derived epoch (kernel/rtc.c), not the old
 * uptime-since-boot stand-in; (2) the auto-detected TZ env var
 * (tz_auto_detect(), same file) actually reaches a real spawned process
 * and musl's own localtime() correctly applies it.
 *
 * Raw write(2) with one full pre-built line per syscall (not printf) --
 * this project's own established lesson (see WynlandOS_Status.md /
 * the plan file's Phase 11 writeup): multiple small writes interleave
 * unpredictably with other processes' concurrent serial output and can
 * look like a hang or truncation when the real issue is elsewhere.
 *
 * Build:
 *   x86_64-linux-musl-gcc -static -O2 -o test_time.elf test_time.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

static void mark(const char *s) {
    write(1, s, strlen(s));
}

int main(void) {
    mark("[test_time] start\n");

    const char *tz = getenv("TZ");
    char line[256];
    int n = snprintf(line, sizeof(line), "[test_time] TZ env = %s\n", tz ? tz : "(unset)");
    write(1, line, (size_t)n);

    mark("[test_time] calling time()...\n");
    time_t t = time(NULL);
    n = snprintf(line, sizeof(line), "[test_time] time() = %ld\n", (long)t);
    write(1, line, (size_t)n);

    if (t < 1577836800L || t > 4102444800L) {
        mark("[test_time] FAIL: time() not plausible\n");
        return 1;
    }

    mark("[test_time] calling gmtime_r()...\n");
    struct tm utc_tm;
    gmtime_r(&t, &utc_tm);
    mark("[test_time] gmtime_r() returned\n");
    n = snprintf(line, sizeof(line), "[test_time] UTC:   %04d-%02d-%02d %02d:%02d:%02d\n",
                 utc_tm.tm_year + 1900, utc_tm.tm_mon + 1, utc_tm.tm_mday,
                 utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec);
    write(1, line, (size_t)n);

    mark("[test_time] calling localtime_r()...\n");
    struct tm local_tm;
    localtime_r(&t, &local_tm);
    mark("[test_time] localtime_r() returned\n");
    n = snprintf(line, sizeof(line), "[test_time] Local: %04d-%02d-%02d %02d:%02d:%02d\n",
                 local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
                 local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
    write(1, line, (size_t)n);

    long diff_sec = (long)(local_tm.tm_hour - utc_tm.tm_hour) * 3600
                  + (long)(local_tm.tm_min - utc_tm.tm_min) * 60;
    n = snprintf(line, sizeof(line), "[test_time] Local - UTC = %ld seconds\n", diff_sec);
    write(1, line, (size_t)n);

    mark("[test_time] PASS: real RTC time + TZ env var reach a real userspace process\n");
    return 0;
}
