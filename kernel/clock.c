/*
 * WynlandOS - the clock without the PIT
 *
 * The kernel's millisecond clock is the PIT's 1 kHz interrupt (kernel/
 * irq.c). New laptops gate the PIT off (Intel's "8254 clock gating", for
 * the S0ix sleep states): it never ticks, and every wait on the clock --
 * the APIC timer's calibration first -- waited forever. So, after ACPI:
 *
 *   - the TSC's rate from the HPET (its period is in its capabilities
 *     register) or, without one, from CPUID 0x15/0x16 (the processor
 *     says its own); the PIT's ticks only as the last resort
 *   - the PIT watched for a moment by that TSC: when it does not tick,
 *     the boot core's APIC timer becomes the 1 kHz clock, on the PIT's
 *     own vector (kernel/smp.c: lapic_clock_takeover())
 */
#include <wynland/types.h>
#include <wynland/acpi.h>
#include <wynland/vmm.h>

extern void serial_write_string(const char *s);
extern void uint_to_str(uint64_t v, char *buf);
extern uint64_t timer_get_ms(void);

uint64_t g_tsc_hz;              /* 0: not known (the PIT then calibrates) */
bool g_pit_dead;                /* the PIT does not tick: the APIC timer is the clock */

static inline uint64_t rdtsc(void) { return __builtin_ia32_rdtsc(); }

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static void log_hz(const char *how, uint64_t hz)
{
    char b[24];
    serial_write_string("Clock: TSC ");
    uint_to_str(hz / 1000000, b);
    serial_write_string(b);
    serial_write_string(" MHz (");
    serial_write_string(how);
    serial_write_string(")\r\n");
}

/* The HPET's main counter against the TSC, 10 ms of it */
static uint64_t tsc_by_hpet(void)
{
    const uint8_t *t = (const uint8_t *)acpi_find_table("HPET");
    if (!t) return 0;
    uint64_t base = *(const uint64_t *)(t + 44);   /* the GAS's address */
    if (!base || t[40] != 0) return 0;             /* system memory only */
    volatile uint64_t *h = (volatile uint64_t *)vmm_map_mmio(base, 0x400);
    if (!h) return 0;
    uint64_t period_fs = h[0] >> 32;               /* GCAP_ID: femtoseconds a tick */
    if (!period_fs || period_fs > 100000000ull) return 0;   /* the spec: <= 100 ns */
    if (!(h[2] & 1)) h[2] |= 1;                    /* GEN_CONF: the main counter on */
    uint64_t want = 10000000000000ull / period_fs; /* ticks in 10 ms */
    uint64_t c0 = h[30], t0 = rdtsc();             /* MAIN_COUNTER at 0xF0 */
    uint64_t c1, t1, guard = 0;
    do {
        c1 = h[30];
        t1 = rdtsc();
        if (++guard > 200000000ull) return 0;      /* a counter that does not count */
    } while (c1 - c0 < want);
    uint64_t ns = (c1 - c0) * (period_fs / 1000) / 1000;
    return ns ? (t1 - t0) * 1000000000ull / ns : 0;
}

/* What the processor says: CPUID 0x15 (TSC / crystal ratio and the
   crystal's rate), else 0x16's base frequency */
static uint64_t tsc_by_cpuid(void)
{
    uint32_t a, b, c, d, max;
    cpuid(0, &max, &b, &c, &d);
    if (max >= 0x15) {
        cpuid(0x15, &a, &b, &c, &d);
        if (a && b) {
            uint64_t crystal = c;
            if (!crystal) {
                /* not given: Intel's own value for its client parts since
                   Ice Lake / Tiger Lake (38.4 MHz), 24 MHz before */
                uint32_t fa, fb, fc, fd;
                cpuid(1, &fa, &fb, &fc, &fd);
                uint32_t model = ((fa >> 4) & 0xF) | ((fa >> 12) & 0xF0);
                crystal = (model == 0x7D || model == 0x7E || model == 0x8C || model == 0x8D ||
                           model == 0x97 || model == 0x9A || model == 0xA7 || model == 0xBA)
                          ? 38400000ull : 24000000ull;
            }
            return crystal * b / a;
        }
    }
    if (max >= 0x16) {
        cpuid(0x16, &a, &b, &c, &d);
        if (a & 0xFFFF) return (uint64_t)(a & 0xFFFF) * 1000000ull;
    }
    return 0;
}

void clock_init(void)
{
    uint64_t hz = tsc_by_hpet();
    if (hz) log_hz("by the HPET", hz);
    else if ((hz = tsc_by_cpuid())) log_hz("by CPUID", hz);
    g_tsc_hz = hz;

    /* the PIT: 30 ms by the TSC -- does the millisecond clock move? (Not
       knowing the TSC: a billion cycles, a third of a second or more.) */
    uint64_t m0 = timer_get_ms(), t0 = rdtsc();
    uint64_t span = hz ? hz / 1000 * 30 : 1000000000ull;
    while (rdtsc() - t0 < span) __builtin_ia32_pause();
    uint64_t moved = timer_get_ms() - m0;
    g_pit_dead = moved < 10;
    if (!hz && !g_pit_dead) {
        hz = (rdtsc() - t0) * 1000 / (moved ? moved : 1);   /* by the PIT, then */
        log_hz("by the PIT", hz);
        g_tsc_hz = hz;
    }
    if (!hz && g_pit_dead) {
        hz = g_tsc_hz = 2000000000ull;                    /* a guess: times are off */
        serial_write_string("Clock: no HPET, no CPUID rate, no PIT: the TSC taken as 2 GHz\r\n");
    }
    if (g_pit_dead) {
        extern bool lapic_clock_takeover(uint64_t tsc_hz);
        serial_write_string("Clock: the PIT does not tick (gated off?) -- the APIC timer is the clock\r\n");
        if (!lapic_clock_takeover(hz)) serial_write_string("Clock: the APIC timer would not start: NO CLOCK\r\n");
    }
}
