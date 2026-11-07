/*
 * WynlandOS - the CPU's temperature, from its own digital sensor (as
 * Linux's coretemp / x86_pkg_temp_thermal): no ACPI, no AML.
 *
 * Intel only, and only when CPUID leaf 6 says the sensor is there (a VM
 * usually says not -- the file then is not there, and the shell shows no
 * temperature). The readout is degrees below TjMax, the CPU's throttling
 * point (MSR_TEMPERATURE_TARGET), so temp = TjMax - readout:
 *   IA32_PACKAGE_THERM_STATUS (0x1B1) for the whole package (CPUID.6:EAX
 *   bit 6), else IA32_THERM_STATUS (0x19C) of the core it runs on.
 *
 * /sys/class/thermal/thermal_zone0/{temp,type} (drivers/fs/ext2.c):
 * millidegrees, "x86_pkg_temp".
 */
#include <wynland/types.h>

static inline void cpuid4(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static inline uint64_t rdmsr64(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static int g_have = -1;          /* -1 not looked yet; 0 none; 1 core; 2 package */
static uint32_t g_tjmax;

static void probe(void)
{
    uint32_t a, b, c, d;
    cpuid4(0, &a, &b, &c, &d);
    /* "GenuineIntel", leaf 6 there */
    if (b != 0x756E6547 || d != 0x49656E69 || c != 0x6C65746E || a < 6) { g_have = 0; return; }
    cpuid4(1, &a, &b, &c, &d);
    uint32_t family = (a >> 8) & 0xF, model = ((a >> 4) & 0xF) | (((a >> 16) & 0xF) << 4);
    cpuid4(6, &a, &b, &c, &d);
    if (!(a & 1)) { g_have = 0; return; }        /* no digital thermal sensor */
    /* TjMax: MSR_TEMPERATURE_TARGET bits 23:16 (Core 2 and later; family 6
       models older than Nehalem lack it -- 100 then, as coretemp guesses) */
    g_tjmax = 100;
    if (family == 6 && model >= 0x1A) {
        uint32_t t = (uint32_t)(rdmsr64(0x1A2) >> 16) & 0xFF;
        if (t >= 70 && t <= 125) g_tjmax = t;
    }
    g_have = (a & (1u << 6)) ? 2 : 1;
}

/* the temperature in millidegrees Celsius; false: no sensor */
bool cpu_temp_mdeg(int32_t *out)
{
    if (g_have < 0) probe();
    if (!g_have) return false;
    uint64_t s = rdmsr64(g_have == 2 ? 0x1B1 : 0x19C);
    if (g_have == 1 && !(s & (1ull << 31))) return false;   /* (the core's reading not valid) */
    uint32_t below = (uint32_t)(s >> 16) & 0x7F;
    *out = ((int32_t)g_tjmax - (int32_t)below) * 1000;
    return true;
}
