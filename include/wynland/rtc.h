/*
 * WynlandOS - real wall-clock time: CMOS RTC read at boot, plus
 * auto-detected timezone offset (via IP geolocation, since this OS has
 * no IANA tzdata and the user should never need to hardcode a location
 * into the source tree). See kernel/rtc.c for the real implementation;
 * this header is the only thing kernel/elf.c (envp) and kernel/syscall.c
 * (clock_gettime) need to see.
 */
#pragma once

#include <wynland/types.h>

/* Reads the CMOS RTC once (assumed UTC -- QEMU's default, no
   base=localtime), converts to a real Unix epoch, and records the
   PIT tick count at that moment so later reads can add elapsed time. */
void rtc_init(void);

/* Real, current UTC epoch seconds -- rtc_init()'s reading plus elapsed
   ticks since. This is what SYS_clock_gettime(CLOCK_REALTIME) returns. */
uint64_t rtc_get_unix_time(void);

/* Best-effort: fetches the caller's current UTC offset from a plain-HTTP
   IP-geolocation service (no hardcoded location anywhere in this
   source tree) and fills g_tz_envp_line + writes /etc/timezone. No-op
   (leaves everything at UTC) if there's no network or the request
   fails -- never guesses, per the explicit "wrong could break things"
   concern this was built for. Call after net_dhcp_request()/DNS are up. */
void tz_auto_detect(void);

/* "TZ=UTC<offset>" (POSIX sign convention: west-of-UTC is positive),
   e.g. "TZ=UTC-2" for UTC+2. Defaults to "TZ=UTC0" until/unless
   tz_auto_detect() successfully overwrites it. Read by kernel/elf.c to
   fill the 6th default_envp slot for every newly spawned process --
   real per-process TZ handling (parsing this string into local-time
   conversions) is musl's own job once the env var is set correctly. */
extern char g_tz_envp_line[32];

/* Signed seconds, local = UTC + this. 0 until tz_auto_detect() succeeds. */
extern int32_t g_tz_offset_seconds;
