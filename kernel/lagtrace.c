/*
 * WynlandOS - where a key's way to the screen spends its time (the GPU
 * desktop typed a letter a second late, and no single part looked slow).
 * Each step's delay is summed and its worst kept; a line every 2 s on the
 * kernel log while keys come:
 *   key   a key in (IRQ) -> the display server read it (/dev/input/kbd)
 *   draw  that read -> its next page flip asked (the frame drawn)
 *   flip  the page flip asked -> done (the display took it)
 *   evt   done -> the display server read the flip event
 */
#include <wynland/types.h>

extern void serial_write_string(const char *s);
extern void uint_to_str(uint64_t v, char *b);
extern uint64_t timer_get_ms(void);
#define timer_ms timer_get_ms()

typedef struct { uint64_t sum, worst, n; } Step;
static Step g_key, g_draw, g_flip, g_evt;
static volatile uint64_t g_key_in;                    /* the oldest key not read yet (0: none) */
static volatile uint64_t g_read_at;                   /* a key read, no flip since (0: none) */
static uint64_t g_since;

static void add(Step *s, uint64_t d) { s->sum += d; s->n++; if (d > s->worst) s->worst = d; }

void lag_key_in(void)    { if (!g_key_in) g_key_in = timer_ms ? timer_ms : 1; }
void lag_key_read(void)
{
    if (!g_key_in) return;
    add(&g_key, timer_ms - g_key_in);
    g_key_in = 0;
    if (!g_read_at) g_read_at = timer_ms ? timer_ms : 1;
}
void lag_flip_asked(void)
{
    if (!g_read_at) return;
    add(&g_draw, timer_ms - g_read_at);
    g_read_at = 0;
}
void lag_flip_done(uint64_t asked_ms) { add(&g_flip, timer_ms - asked_ms); }
void lag_event_read(uint64_t done_ms) { add(&g_evt, timer_ms - done_ms); }

static void line(const char *name, const Step *s)
{
    char b[24];
    serial_write_string(" ");
    serial_write_string(name);
    serial_write_string(" avg ");
    uint_to_str(s->n ? s->sum / s->n : 0, b); serial_write_string(b);
    serial_write_string(" max ");
    uint_to_str(s->worst, b); serial_write_string(b);
    serial_write_string(" n ");
    uint_to_str(s->n, b); serial_write_string(b);
}

/* from a syscall (not an interrupt): the line, every 2 s while keys came */
void lag_report(void)
{
    extern void stall_report(void);
    stall_report();
    if (timer_ms - g_since < 2000) return;
    g_since = timer_ms;
    if (!g_key.n) { g_draw.n = g_flip.n = g_evt.n = 0; return; }
    serial_write_string("LAG (ms):");
    line("key", &g_key); line("draw", &g_draw); line("flip", &g_flip); line("evt", &g_evt);
    serial_write_string("\r\n");
    Step z = { 0, 0, 0 };
    g_key = g_draw = g_flip = g_evt = z;
}
