/*
 * WynlandOS - real wall-clock time.
 *
 * Two genuinely separate concerns, kept separate on purpose:
 *  1. rtc_init()/rtc_get_unix_time() -- a real CMOS RTC read at boot gives
 *     an absolute UTC epoch (QEMU's default RTC base is UTC, no
 *     `-rtc base=localtime` in this project's run.sh, so no conversion
 *     needed there). This alone fixes the long-documented "OS thinks it's
 *     some default past time" gap that made HTTPS cert-date validation
 *     fail (see curl's own Phase 17 writeup).
 *  2. tz_auto_detect() -- a *local* offset for userspace's own
 *     localtime()/mktime() to use, auto-detected via IP geolocation
 *     rather than ever hardcoding a location into this source tree (the
 *     owner explicitly doesn't want a personal location baked into a
 *     public repo). Best-effort and honest about it: on any failure this
 *     leaves the system at UTC rather than guessing wrong.
 */
#include <wynland/types.h>
#include <wynland/rtc.h>
#include <wynland/vfs.h>
#include <wynland/http.h>

extern void serial_write_string(const char *str);
extern uint64_t timer_get_ticks(void); /* PIT, 100Hz -- kernel/irq.c */

char g_tz_envp_line[32] = "TZ=UTC0";
int32_t g_tz_offset_seconds = 0;

static uint64_t g_boot_unix_time = 0;
static uint64_t g_boot_tick_baseline = 0;

/* ---------------- CMOS RTC read ---------------- */

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71
#define CMOS_REG_SECONDS 0x00
#define CMOS_REG_MINUTES 0x02
#define CMOS_REG_HOURS   0x04
#define CMOS_REG_DAY     0x07
#define CMOS_REG_MONTH   0x08
#define CMOS_REG_YEAR    0x09
#define CMOS_REG_STATUS_A 0x0A
#define CMOS_REG_STATUS_B 0x0B

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(uint16_t port, uint8_t data) {
    __asm__ volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static bool cmos_update_in_progress(void) {
    return (cmos_read(CMOS_REG_STATUS_A) & 0x80) != 0;
}

/* Reads all 6 fields, retrying until two consecutive reads agree -- the
   standard technique to avoid tearing against the RTC's own once-a-second
   update (real hardware has no atomic multi-register read). */
static void cmos_read_registers(uint8_t out[6]) {
    uint8_t prev[6];
    for (;;) {
        while (cmos_update_in_progress()) {}
        out[0] = cmos_read(CMOS_REG_SECONDS);
        out[1] = cmos_read(CMOS_REG_MINUTES);
        out[2] = cmos_read(CMOS_REG_HOURS);
        out[3] = cmos_read(CMOS_REG_DAY);
        out[4] = cmos_read(CMOS_REG_MONTH);
        out[5] = cmos_read(CMOS_REG_YEAR);

        while (cmos_update_in_progress()) {}
        prev[0] = cmos_read(CMOS_REG_SECONDS);
        prev[1] = cmos_read(CMOS_REG_MINUTES);
        prev[2] = cmos_read(CMOS_REG_HOURS);
        prev[3] = cmos_read(CMOS_REG_DAY);
        prev[4] = cmos_read(CMOS_REG_MONTH);
        prev[5] = cmos_read(CMOS_REG_YEAR);

        bool same = true;
        for (int i = 0; i < 6; i++) if (out[i] != prev[i]) { same = false; break; }
        if (same) return;
    }
}

static uint8_t bcd_to_bin(uint8_t v) { return (uint8_t)((v & 0x0F) + (v >> 4) * 10); }

/* Howard Hinnant's days-from-civil (public domain), correct across the
   whole proleptic Gregorian calendar -- returns days since 1970-01-01. */
static int64_t days_from_civil(int64_t y, uint32_t m, uint32_t d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void rtc_init(void) {
    uint8_t regs[6];
    cmos_read_registers(regs);
    uint8_t status_b = cmos_read(CMOS_REG_STATUS_B);
    bool is_bcd = !(status_b & 0x04);
    bool is_12h = !(status_b & 0x02);

    uint8_t sec = regs[0], min = regs[1], hour = regs[2];
    uint8_t day = regs[3], month = regs[4], year = regs[5];
    bool pm = is_12h && (hour & 0x80);
    hour &= 0x7F;

    if (is_bcd) {
        sec = bcd_to_bin(sec);
        min = bcd_to_bin(min);
        hour = bcd_to_bin(hour);
        day = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year = bcd_to_bin(year);
    }
    if (is_12h) {
        if (pm && hour != 12) hour = (uint8_t)(hour + 12);
        if (!pm && hour == 12) hour = 0;
    }

    /* No reliable CMOS century register across QEMU/real-BIOS
       configurations -- assume 2000+year, true for any boot this
       project will ever realistically see. */
    int64_t full_year = 2000 + year;
    int64_t days = days_from_civil(full_year, month, day);
    g_boot_unix_time = (uint64_t)(days * 86400LL + hour * 3600 + min * 60 + sec);
    g_boot_tick_baseline = timer_get_ticks();

    serial_write_string("RTC: read real UTC time from CMOS, epoch=");
    char buf[24];
    extern void uint_to_str(uint64_t val, char *buf);
    uint_to_str(g_boot_unix_time, buf);
    serial_write_string(buf);
    serial_write_string("\r\n");
}

uint64_t rtc_get_unix_time(void) {
    uint64_t elapsed_ticks = timer_get_ticks() - g_boot_tick_baseline;
    return g_boot_unix_time + elapsed_ticks / 100;
}

/* ---------------- timezone auto-detection ---------------- */

void tz_auto_detect(void) {
    char resp_buf[512];
    HttpResponse resp;
    /* ip-api.com's free tier serves plain HTTP (no TLS needed -- this
       runs before we'd fully trust cert-date validation anyway) and its
       "line" format returns one requested field per line, no JSON
       parsing needed. `offset` is the CURRENT UTC offset in seconds,
       already DST-adjusted server-side -- exactly what's needed without
       implementing DST transition rules locally. */
    int ret = http_get("ip-api.com", 80, "/line/?fields=offset,timezone",
                        resp_buf, sizeof(resp_buf), &resp);
    if (ret < 0 || resp.status_code != 200 || resp.body_len == 0) {
        serial_write_string("RTC/TZ: geo-IP auto-detect unavailable (no network or request failed), staying on UTC\r\n");
        return;
    }
    uint32_t body_len = resp.body_len < sizeof(resp_buf) - 1 ? resp.body_len : sizeof(resp_buf) - 1;
    resp_buf[body_len] = '\0';

    /* ip-api.com's "line" format returns fields in its own fixed
       canonical order, NOT the order listed in `fields=` -- confirmed
       empirically (`fields=offset,timezone` still returns the timezone
       name first, then the numeric offset second). Skip past the first
       line (the IANA zone name, purely informational -- just logged)
       rather than assume the response starts with the number. */
    uint32_t i = 0;
    while (resp_buf[i] != '\0' && resp_buf[i] != '\n') i++;
    if (i > 0) {
        char saved = resp_buf[i];
        resp_buf[i] = '\0';
        serial_write_string("RTC/TZ: geo-IP reports zone ");
        serial_write_string(resp_buf);
        serial_write_string("\r\n");
        resp_buf[i] = saved;
    }
    if (resp_buf[i] == '\n') i++;

    bool neg = false;
    if (resp_buf[i] == '-') { neg = true; i++; }
    else if (resp_buf[i] == '+') { i++; }
    if (resp_buf[i] < '0' || resp_buf[i] > '9') {
        serial_write_string("RTC/TZ: unexpected geo-IP response format, staying on UTC\r\n");
        return;
    }
    int32_t val = 0;
    while (resp_buf[i] >= '0' && resp_buf[i] <= '9') {
        val = val * 10 + (resp_buf[i] - '0');
        i++;
    }
    if (neg) val = -val;

    /* Real UTC offsets range -12:00..+14:00 -- reject anything outside
       that as a malformed/unexpected response rather than ever applying
       a wrong offset (an explicit "wrong could break things" concern). */
    if (val < -12 * 3600 || val > 14 * 3600) {
        serial_write_string("RTC/TZ: geo-IP offset out of plausible range, staying on UTC\r\n");
        return;
    }

    g_tz_offset_seconds = val;

    /* POSIX TZ sign convention is inverted from common expectation: the
       offset field is "add this to local time to get UTC", so UTC+2
       (val=+7200) is written as TZ=UTC-2. */
    int32_t posix_total = -val;
    bool posix_neg = posix_total < 0;
    int32_t abs_total = posix_neg ? -posix_total : posix_total;
    int32_t posix_hours = abs_total / 3600;
    int32_t posix_minutes = (abs_total % 3600) / 60;

    char *p = g_tz_envp_line;
    const char *prefix = "TZ=UTC";
    for (const char *s = prefix; *s; s++) *p++ = *s;
    if (posix_neg) *p++ = '-';
    if (posix_hours == 0) {
        *p++ = '0';
    } else {
        char rev[8]; int rn = 0; int32_t tmp = posix_hours;
        while (tmp > 0) { rev[rn++] = (char)('0' + (tmp % 10)); tmp /= 10; }
        while (rn > 0) *p++ = rev[--rn];
    }
    if (posix_minutes != 0) {
        *p++ = ':';
        *p++ = (char)('0' + (posix_minutes / 10));
        *p++ = (char)('0' + (posix_minutes % 10));
    }
    *p = '\0';

    serial_write_string("RTC/TZ: auto-detected offset, ");
    serial_write_string(g_tz_envp_line);
    serial_write_string("\r\n");

    /* /etc/timezone stores just the zone string (no "TZ=" prefix),
       matching the real-Linux /etc/timezone convention closely enough
       for this OS's purposes -- refreshed every boot, same reasoning as
       /etc/resolv.conf's own "always overwrite, don't gate on first
       creation" pattern. */
    vfs_create("/etc/timezone");
    VfsFile *tf = vfs_open_flags("/etc/timezone", VFS_O_WRITE | VFS_O_TRUNC);
    if (tf) {
        char line[40];
        int ln = 0;
        for (const char *s = g_tz_envp_line + 3; *s; s++) line[ln++] = *s; /* skip "TZ=" */
        line[ln++] = '\n';
        vfs_write(tf, line, (uint32_t)ln);
        vfs_close(tf);
    }
}
