/*
 * WynlandOS - the kernel log over the network (boot.cfg "netlog=IP:PORT").
 *
 * For a real machine without a serial port: every byte the kernel logs
 * (serial_write_char) also lands in a ring here, and a kernel thread sends
 * what is new as UDP datagrams every 50 ms -- from the start of the boot,
 * once the network is up (a phone's USB tethering will do). A receiver on
 * the developer's machine (tools/devhost.py) writes it to a file, so the
 * last lines before a hang are there even when the screen is not. A panic
 * sends what is left before it resets the machine.
 */
#include <wynland/types.h>
#include <wynland/net.h>
#include <wynland/sched.h>
#include <wynland/spinlock.h>

#define NL_BYTES    (256u * 1024u)
#define NL_DGRAM    1200u
#define NL_SRCPORT  5140

static char g_nl[NL_BYTES];
static volatile uint64_t g_nl_head;     /* bytes ever logged */
static uint64_t g_nl_tail;              /* bytes ever sent */
static uint32_t g_nl_ip;                /* network order; 0: off */
static uint16_t g_nl_port;
static volatile int g_nl_busy;

void netlog_putc(char c)
{
    uint64_t i = __sync_fetch_and_add(&g_nl_head, 1);
    g_nl[i % NL_BYTES] = c;
}

/* what is new, as datagrams; false: the NIC is busy, the rest later */
static bool nl_flush(void)
{
    if (!g_nl_ip || !net_is_up()) return true;
    if (__sync_lock_test_and_set(&g_nl_busy, 1)) return true;
    static char pkt[NL_DGRAM];
    uint64_t head = g_nl_head;
    bool all = true;
    if (head - g_nl_tail > NL_BYTES) g_nl_tail = head - NL_BYTES;   /* overrun: the oldest lost */
    while (g_nl_tail < head) {
        uint32_t n = head - g_nl_tail > NL_DGRAM ? NL_DGRAM : (uint32_t)(head - g_nl_tail);
        for (uint32_t i = 0; i < n; i++) pkt[i] = g_nl[(g_nl_tail + i) % NL_BYTES];
        if (!net_send_udp(g_nl_ip, NL_SRCPORT, g_nl_port, pkt, n)) { all = false; break; }
        g_nl_tail += n;
    }
    __sync_lock_release(&g_nl_busy);
    return all;
}

static void netlog_thread(void *arg)
{
    (void)arg;
    for (;;) sched_sleep_ms(nl_flush() ? 50 : 2);
}

/* boot.cfg's text: "netlog=a.b.c.d:port" starts it (once) */
void netlog_start(const char *cfg)
{
    static bool started;
    if (started) return;
    for (int i = 0; cfg[i]; i++) {
        if (i && cfg[i - 1] != '\n') continue;
        const char *k = "netlog=";
        int j = 0;
        while (k[j] && cfg[i + j] == k[j]) j++;
        if (k[j]) continue;
        const char *q = cfg + i + j;
        uint32_t ip = 0, port = 0;
        for (int b = 0; b < 4; b++) {
            uint32_t v = 0;
            if (*q < '0' || *q > '9') return;
            while (*q >= '0' && *q <= '9') v = v * 10 + (uint32_t)(*q++ - '0');
            if (v > 255 || (b < 3 && *q++ != '.')) return;
            ip |= v << (8 * b);                 /* network order in memory */
        }
        if (*q++ != ':') return;
        while (*q >= '0' && *q <= '9') port = port * 10 + (uint32_t)(*q++ - '0');
        if (!port || port > 65535) return;
        g_nl_ip = ip;
        g_nl_port = (uint16_t)port;
        started = true;
        thread_create(netlog_thread, NULL);
        return;
    }
}

/* a panic: what is left, now -- unless the USB driver was in the middle of
   something (its lock held: it may be what crashed) */
void netlog_panic_flush(void)
{
    extern Spinlock g_usb_lock;
    if (!g_nl_ip || spin_held(&g_usb_lock)) return;
    extern void usb_drain(void);
    for (int i = 0; i < 400 && !nl_flush(); i++) {
        usb_drain();                              /* sent ones free their buffers */
        for (volatile int k = 0; k < 200000; k++) __asm__ volatile("pause");
    }
}
