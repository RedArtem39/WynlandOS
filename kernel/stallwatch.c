/*
 * WynlandOS - who stops the boot core (Linux's NMI watchdog, for finding
 * them rather than for panicking). The boot core runs the clock: with its
 * interrupts off for long, no millisecond sleeper wakes and no key is
 * delivered -- the GPU desktop read keys 150 ms late, "LATENCY" lines said
 * 100-1000 ms, and the culprit's name was nowhere.
 *
 * An application core watches the clock on its own 1 kHz tick; when it has
 * not moved for 30 ms, the boot core gets an NMI -- delivered with
 * interrupts off -- and the NMI's handler writes down where it was: the
 * instruction and the kernel return addresses on its stack. Printed later,
 * from a syscall, with the kernel's symbol names (kernel/module.c's table):
 *   STALL cpu0 30+ ms at func+0x1c <- caller+0x40 <- ...
 */
#include <wynland/types.h>
#include <wynland/idt.h>
#include <wynland/memlayout.h>
#include <wynland/smp.h>

extern void serial_write_string(const char *s);
extern void uint_to_hex(uint64_t v, char *buf);
extern uint64_t timer_get_ms(void);
extern const char *ksym_name(uint64_t addr, uint64_t *off);
extern char __text_end[];
extern void smp_nmi_cpu(uint32_t cpu);

#define NSAMPLE 8
#define NCALLER 8
typedef struct { uint64_t at_ms, rip, caller[NCALLER]; uint32_t n; bool user; } Sample;
static Sample g_s[NSAMPLE];
static volatile uint32_t g_head, g_tail;
static volatile bool g_armed;                         /* an NMI sent, not handled yet */

/* the NMI, on the boot core */
void stall_nmi(InterruptRegisters *r)
{
    g_armed = false;
    uint32_t h = g_head;
    if (h - g_tail >= NSAMPLE) return;
    Sample *s = &g_s[h % NSAMPLE];
    s->at_ms = timer_get_ms();
    s->rip = r->rip;
    s->user = (r->cs & 3) != 0;
    s->n = 0;
    if (!s->user && r->rsp >= HHDM_BASE) {
        const uint64_t *sp = (const uint64_t *)(uintptr_t)r->rsp;
        for (int i = 0; i < 512 && s->n < NCALLER; i++) {
            uint64_t w = sp[i];
            if (w >= KERNEL_VMA + KERNEL_PHYS && w < (uint64_t)(uintptr_t)__text_end) s->caller[s->n++] = w;
        }
    }
    g_head = h + 1;
}

/* an application core's tick: is the clock moving? */
void stall_watch_tick(void)
{
    static uint64_t last;
    static uint32_t same;
    uint64_t now = timer_get_ms();
    if (now != last) { last = now; same = 0; return; }
    if (++same == 30 && !g_armed) {
        g_armed = true;
        smp_nmi_cpu(0);
    }
}

static void name(uint64_t a)
{
    char b[20];
    uint64_t off = 0;
    const char *n = ksym_name(a, &off);
    if (n && off < 0x10000) {
        serial_write_string(n);
        serial_write_string("+");
        uint_to_hex(off, b);
    } else {
        uint_to_hex(a, b);
    }
    serial_write_string(b);
}

/* from a syscall: what the NMIs caught */
void stall_report(void)
{
    while (g_tail != g_head) {
        Sample *s = &g_s[g_tail % NSAMPLE];
        serial_write_string("STALL cpu0 30+ ms at ");
        if (s->user) serial_write_string("(user code) ");
        name(s->rip);
        for (uint32_t i = 0; i < s->n; i++) { serial_write_string(" <- "); name(s->caller[i]); }
        serial_write_string("\r\n");
        g_tail++;
    }
}
