/*
 * WynlandOS - Intel High Definition Audio, output only.
 * ============================================================
 * Controller (PCI class 04:03) programmed through MMIO with polled
 * CORB/RIRB command rings; the codec walked for an audio function group,
 * its first output converter (DAC) and the output pins, routed DAC -> pin
 * directly or through one mixer/selector. One output stream plays a
 * cyclic DMA ring; programs reach it as an OSS /dev/dsp.
 *
 * Underruns: every timer tick zeroes what the hardware has just played,
 * so a program that stops writing leaves silence behind instead of the
 * ring's old contents looping.
 */
#include <wynland/hda.h>
#include <wynland/pci.h>
#include <wynland/pmm.h>
#include <wynland/vmm.h>
#include <wynland/sched.h>
#include <wynland/usercopy.h>

extern void serial_write_string(const char *s);
extern void uint_to_hex(uint64_t v, char *buf);
extern uint64_t timer_get_ms(void);
extern void *pmm_alloc_contiguous(uint32_t count);

/* ---- controller registers ---- */
#define GCAP      0x00
#define GCTL      0x08
#define STATESTS  0x0E
#define INTCTL    0x20
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP    0x48
#define CORBRP    0x4A
#define CORBCTL   0x4C
#define CORBSIZE  0x4E
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP    0x58
#define RINTCNT   0x5A
#define RIRBCTL   0x5C
#define RIRBSTS   0x5D
#define RIRBSIZE  0x5E
#define SD_BASE   0x80
#define SD_SIZE   0x20
/* stream descriptor offsets */
#define SD_CTL0   0x00
#define SD_CTL2   0x02
#define SD_STS    0x03
#define SD_LPIB   0x04
#define SD_CBL    0x08
#define SD_LVI    0x0C
#define SD_FMT    0x12
#define SD_BDPL   0x18
#define SD_BDPU   0x1C

/* The ring is far bigger than what may be queued in it. The kernel can
   run with interrupts off for a long time (a syscall loading a big
   library from disk), and during such a stall the card goes on playing
   while nothing zeroes what it played: with a ring shorter than the stall
   it came round to the old data and played it again. 1 MB is 5.4 s of
   48 kHz stereo; at most MAX_QUEUED of it holds audio, which keeps the
   latency at half a second. */
#define RING_PAGES   256
#define RING_BYTES   (RING_PAGES * 4096u)
#define MAX_QUEUED   (96u * 1024)            /* ~0.5 s at 48 kHz stereo */
#define BDL_ENTRIES  4
#define STREAM_TAG   1
#define WRITE_LEAD   8192u                   /* where a fresh write starts, ahead of the hardware: QEMU pulls in bursts */

static volatile uint8_t *g_mmio;
static bool      g_ok;
static uint32_t *g_corb;
static uint32_t *g_rirb;
static uint16_t  g_corb_n, g_rirb_n, g_rirb_rp;
static uint8_t   g_cad;
static uint32_t  g_dac;
static uint32_t  g_afg;
static uint32_t  g_sd;                       /* our output stream descriptor */
static uint8_t  *g_ring;                     /* identity-mapped, physically contiguous */
static uint64_t *g_bdl;

/* playback state (touched by write()/ioctl() with interrupts off and by
   the timer tick) */
static uint32_t g_wpos;                      /* next byte the writer fills */
static uint32_t g_last_lpib;
static uint32_t g_pending;                   /* queued ahead of the hardware: PCM plus the
                                                lead-in silence before it */
static bool     g_idle = true;               /* nothing queued: the next write starts fresh */
static uint32_t g_rate = 48000, g_channels = 2;
static bool     g_running;

static inline uint8_t  r8 (uint32_t o) { return *(volatile uint8_t  *)(g_mmio + o); }
static inline uint16_t r16(uint32_t o) { return *(volatile uint16_t *)(g_mmio + o); }
static inline uint32_t r32(uint32_t o) { return *(volatile uint32_t *)(g_mmio + o); }
static inline void w8 (uint32_t o, uint8_t v)  { *(volatile uint8_t  *)(g_mmio + o) = v; }
static inline void w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(g_mmio + o) = v; }
static inline void w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_mmio + o) = v; }

static void log_hex(const char *what, uint64_t v)
{
    char b[32];
    uint_to_hex(v, b);
    serial_write_string(what);
    serial_write_string(b);
    serial_write_string("\r\n");
}

/* busy wait, usable before and after interrupts are on */
static void delay_us(uint32_t us)
{
    for (volatile uint32_t i = 0; i < us * 40; i++) __asm__ volatile("pause");
}

static bool wait_bit16(uint32_t o, uint16_t mask, uint16_t want, uint32_t us)
{
    for (uint32_t i = 0; i < us / 10 + 1; i++) {
        if ((r16(o) & mask) == want) return true;
        delay_us(10);
    }
    return false;
}

/* ---- codec commands ---- */

static bool cmd(uint32_t verb, uint32_t *resp)
{
    uint16_t wp = (uint16_t)((r16(CORBWP) + 1) % g_corb_n);
    g_corb[wp] = verb;
    __asm__ volatile("mfence" ::: "memory");
    w16(CORBWP, wp);
    for (uint32_t i = 0; i < 2000; i++) {          /* ~20 ms */
        uint16_t rwp = r16(RIRBWP) & 0xFF;
        if (rwp != g_rirb_rp) {
            g_rirb_rp = (uint16_t)((g_rirb_rp + 1) % g_rirb_n);
            if (resp) *resp = g_rirb[g_rirb_rp * 2];
            w8(RIRBSTS, 0x5);                       /* ack */
            return true;
        }
        delay_us(10);
    }
    log_hex("HDA: no response to verb 0x", verb);
    return false;
}

static uint32_t verb12(uint32_t nid, uint32_t v, uint32_t payload8)
{
    return ((uint32_t)g_cad << 28) | (nid << 20) | (v << 8) | (payload8 & 0xFF);
}

static uint32_t verb4(uint32_t nid, uint32_t v, uint32_t payload16)
{
    return ((uint32_t)g_cad << 28) | (nid << 20) | (v << 16) | (payload16 & 0xFFFF);
}

static uint32_t param(uint32_t nid, uint32_t p)
{
    uint32_t r = 0;
    cmd(verb12(nid, 0xF00, p), &r);
    return r;
}

static void set(uint32_t nid, uint32_t v, uint32_t payload8) { cmd(verb12(nid, v, payload8), 0); }
static void set4(uint32_t nid, uint32_t v, uint32_t payload16) { cmd(verb4(nid, v, payload16), 0); }

/* connection list entry i of nid (short form only) */
static uint32_t conn_entry(uint32_t nid, uint32_t i)
{
    uint32_t r = 0;
    cmd(verb12(nid, 0xF02, i & ~3u), &r);
    return (r >> ((i & 3) * 8)) & 0xFF;
}

static uint32_t conn_count(uint32_t nid)
{
    uint32_t l = param(nid, 0x0E);
    if (l & 0x80) return 0;                       /* long form: not supported */
    return l & 0x7F;
}

static int conn_index(uint32_t nid, uint32_t target)
{
    uint32_t n = conn_count(nid);
    for (uint32_t i = 0; i < n; i++)
        if (conn_entry(nid, i) == target) return (int)i;
    return -1;
}

/* unmute an amplifier at 0 dB (the codec's own offset step) */
static void unmute_out(uint32_t nid, uint32_t afg)
{
    uint32_t caps = param(nid, 0x12);
    if (!caps) caps = param(afg, 0x12);
    uint32_t gain = caps & 0x7F;
    set4(nid, 0x3, 0xB000 | gain);                /* output, left+right, unmuted */
}

static void unmute_in(uint32_t nid, uint32_t idx, uint32_t afg)
{
    uint32_t caps = param(nid, 0x0D);
    if (!caps) caps = param(afg, 0x0D);
    uint32_t gain = caps & 0x7F;
    set4(nid, 0x3, 0x7000 | ((idx & 0xF) << 8) | gain);   /* input idx, left+right */
}

static uint16_t fmt_word(uint32_t rate, uint32_t ch)
{
    uint16_t f = (uint16_t)(0x10 | ((ch - 1) & 0xF));      /* 16-bit */
    if (rate == 44100) f |= 0x4000;                         /* 44.1 kHz base */
    return f;
}

/* ---- stream ---- */

static void stream_stop(void)
{
    w8(g_sd + SD_CTL0, r8(g_sd + SD_CTL0) & ~0x02);
    for (int i = 0; i < 1000 && (r8(g_sd + SD_CTL0) & 0x02); i++) delay_us(10);
    g_running = false;
}

static void stream_start(void)
{
    uint16_t f = fmt_word(g_rate, g_channels);
    /* reset the descriptor */
    w8(g_sd + SD_CTL0, 0x01);
    for (int i = 0; i < 1000 && !(r8(g_sd + SD_CTL0) & 0x01); i++) delay_us(10);
    w8(g_sd + SD_CTL0, 0x00);
    for (int i = 0; i < 1000 && (r8(g_sd + SD_CTL0) & 0x01); i++) delay_us(10);

    uint64_t bdl = (uint64_t)(uintptr_t)g_bdl;
    w32(g_sd + SD_BDPL, (uint32_t)bdl);
    w32(g_sd + SD_BDPU, (uint32_t)(bdl >> 32));
    w32(g_sd + SD_CBL, RING_BYTES);
    w16(g_sd + SD_LVI, BDL_ENTRIES - 1);
    w16(g_sd + SD_FMT, f);
    w8(g_sd + SD_CTL2, (uint8_t)(STREAM_TAG << 4));

    set4(g_dac, 0x2, f);                                   /* converter format */
    set(g_dac, 0x706, (STREAM_TAG << 4) | 0);              /* stream tag, channel 0 */
    /* volume/mute again, after the format: a codec may (QEMU's does)
       rebuild its output channel on a format change and apply the
       amplifier only on the next amp command */
    unmute_out(g_dac, g_afg);

    for (uint32_t i = 0; i < RING_BYTES / 8; i++) ((uint64_t *)g_ring)[i] = 0;
    g_wpos = WRITE_LEAD;
    g_pending = 0;
    g_idle = true;
    g_last_lpib = 0;
    w8(g_sd + SD_CTL0, 0x02);                              /* RUN */
    g_running = true;
}

/* ---- init ---- */

static bool setup_rings(void)
{
    void *cp = pmm_alloc_page(), *rp = pmm_alloc_page();
    if (!cp || !rp) return false;
    g_corb = (uint32_t *)cp;
    g_rirb = (uint32_t *)rp;
    for (int i = 0; i < 1024; i++) { g_corb[i] = 0; g_rirb[i] = 0; }

    w8(CORBCTL, 0);
    w8(RIRBCTL, 0);
    delay_us(100);

    uint8_t cs = r8(CORBSIZE);
    if (cs & 0x40)      { w8(CORBSIZE, 0x2); g_corb_n = 256; }
    else if (cs & 0x20) { w8(CORBSIZE, 0x1); g_corb_n = 16; }
    else                { w8(CORBSIZE, 0x0); g_corb_n = 2; }
    uint8_t rs = r8(RIRBSIZE);
    if (rs & 0x40)      { w8(RIRBSIZE, 0x2); g_rirb_n = 256; }
    else if (rs & 0x20) { w8(RIRBSIZE, 0x1); g_rirb_n = 16; }
    else                { w8(RIRBSIZE, 0x0); g_rirb_n = 2; }

    w32(CORBLBASE, (uint32_t)(uintptr_t)cp);
    w32(CORBUBASE, (uint32_t)((uint64_t)(uintptr_t)cp >> 32));
    w32(RIRBLBASE, (uint32_t)(uintptr_t)rp);
    w32(RIRBUBASE, (uint32_t)((uint64_t)(uintptr_t)rp >> 32));

    /* reset the CORB read pointer (some controllers never echo the bit) */
    w16(CORBRP, 0x8000);
    wait_bit16(CORBRP, 0x8000, 0x8000, 1000);
    w16(CORBRP, 0x0000);
    wait_bit16(CORBRP, 0x8000, 0x0000, 1000);
    w16(CORBWP, 0);

    w16(RIRBWP, 0x8000);                                   /* reset RIRB write pointer */
    w16(RINTCNT, 1);
    g_rirb_rp = 0;

    w8(CORBCTL, 0x02);                                     /* CORB DMA run */
    /* DMA run + response "interrupt" control: with it the controller
       raises RINTFL every RINTCNT responses and waits for the driver to
       clear it before taking more commands; without it nothing ever
       raises the flag and command processing stops after the first
       response. INTCTL stays 0, so no interrupt is actually delivered. */
    w8(RIRBCTL, 0x03);
    delay_us(100);
    return true;
}

static bool route_codec(void)
{
    uint32_t root = param(0, 0x04);
    uint32_t fg_start = (root >> 16) & 0xFF, fg_n = root & 0xFF;
    uint32_t afg = 0;
    for (uint32_t n = fg_start; n < fg_start + fg_n; n++)
        if ((param(n, 0x05) & 0xFF) == 0x01) { afg = n; break; }
    if (!afg) { serial_write_string("HDA: no audio function group\r\n"); return false; }

    g_afg = afg;
    set(afg, 0x705, 0);                                    /* power D0 */
    delay_us(1000);

    uint32_t wr = param(afg, 0x04);
    uint32_t ws = (wr >> 16) & 0xFF, wn = wr & 0xFF;

    g_dac = 0;
    for (uint32_t n = ws; n < ws + wn; n++) {
        uint32_t caps = param(n, 0x09);
        if (((caps >> 20) & 0xF) == 0x0) { g_dac = n; break; }
    }
    if (!g_dac) { serial_write_string("HDA: no output converter\r\n"); return false; }
    set(g_dac, 0x705, 0);
    unmute_out(g_dac, afg);

    int pins = 0;
    for (uint32_t n = ws; n < ws + wn; n++) {
        uint32_t caps = param(n, 0x09);
        if (((caps >> 20) & 0xF) != 0x4) continue;          /* pin complex */
        uint32_t pcaps = param(n, 0x0C);
        if (!(pcaps & 0x10)) continue;                      /* not output capable */
        uint32_t cfg = 0;
        cmd(verb12(n, 0xF1C, 0), &cfg);
        if (((cfg >> 30) & 3) == 1) continue;               /* nothing connected */

        /* DAC -> pin, directly or through one mixer/selector */
        int idx = conn_index(n, g_dac);
        if (idx < 0) {
            uint32_t cn = conn_count(n);
            for (uint32_t i = 0; i < cn && idx < 0; i++) {
                uint32_t mid = conn_entry(n, i);
                uint32_t mtype = (param(mid, 0x09) >> 20) & 0xF;
                if (mtype != 0x2 && mtype != 0x3) continue;   /* mixer / selector */
                int j = conn_index(mid, g_dac);
                if (j < 0) continue;
                set(mid, 0x705, 0);
                if (mtype == 0x3) set(mid, 0x701, (uint32_t)j);
                unmute_in(mid, (uint32_t)j, afg);
                unmute_out(mid, afg);
                idx = (int)i;
            }
        }
        if (idx < 0) continue;
        set(n, 0x705, 0);
        if (conn_count(n) > 1) set(n, 0x701, (uint32_t)idx);
        uint32_t dev = (cfg >> 20) & 0xF;
        set(n, 0x707, dev == 0x2 ? 0xC0 : 0x40);           /* out enable (+HP amp) */
        if (pcaps & 0x10000) set(n, 0x70C, 0x02);           /* EAPD */
        unmute_out(n, afg);
        pins++;
    }
    if (!pins) { serial_write_string("HDA: no usable output pin\r\n"); return false; }
    log_hex("HDA: DAC node 0x", g_dac);
    log_hex("HDA: output pins: 0x", (uint64_t)pins);
    return true;
}

void hda_init(void)
{
    PciDevice d;
    if (!pci_find_device(0x04, 0x03, &d)) return;          /* no HD Audio controller */

    uint32_t bar0 = pci_read_config(d.bus, d.slot, d.func, 0x10);
    uint64_t base = bar0 & ~0xFULL;
    if ((bar0 & 0x6) == 0x4)                                /* 64-bit BAR */
        base |= (uint64_t)pci_read_config(d.bus, d.slot, d.func, 0x14) << 32;
    /* memory space + bus master */
    uint32_t cmdreg = pci_read_config(d.bus, d.slot, d.func, 0x04);
    pci_write_config(d.bus, d.slot, d.func, 0x04, cmdreg | 0x6);
    vmm_map_mmio(base, 0x4000);
    g_mmio = (volatile uint8_t *)(uintptr_t)base;
    log_hex("HDA: controller at 0x", base);

    /* controller reset */
    w32(GCTL, r32(GCTL) & ~1u);
    for (int i = 0; i < 1000 && (r32(GCTL) & 1); i++) delay_us(10);
    delay_us(100);
    w32(GCTL, r32(GCTL) | 1u);
    for (int i = 0; i < 1000 && !(r32(GCTL) & 1); i++) delay_us(10);
    delay_us(1000);                                         /* codecs wake up (>= 521 us) */

    uint16_t codecs = r16(STATESTS);
    if (!codecs) { serial_write_string("HDA: no codec\r\n"); return; }
    for (g_cad = 0; g_cad < 15 && !(codecs & (1u << g_cad)); g_cad++) {}
    w16(STATESTS, codecs);
    w32(INTCTL, 0);                                         /* polled */

    if (!setup_rings()) return;
    if (!route_codec()) return;

    uint16_t gcap = r16(GCAP);
    uint32_t iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    if (!oss) { serial_write_string("HDA: no output stream\r\n"); return; }
    g_sd = SD_BASE + iss * SD_SIZE;                         /* first output stream */

    g_ring = (uint8_t *)pmm_alloc_contiguous(RING_PAGES);
    g_bdl = (uint64_t *)pmm_alloc_page();
    if (!g_ring || !g_bdl) { serial_write_string("HDA: out of memory\r\n"); return; }
    for (int i = 0; i < 512; i++) g_bdl[i] = 0;
    for (uint32_t i = 0; i < BDL_ENTRIES; i++) {
        uint32_t chunk = RING_BYTES / BDL_ENTRIES;
        g_bdl[i * 2]     = (uint64_t)(uintptr_t)g_ring + i * chunk;   /* address */
        g_bdl[i * 2 + 1] = chunk;                                      /* length, IOC=0 */
    }

    stream_start();
    g_ok = true;
    serial_write_string("HDA: playback ready (/dev/dsp)\r\n");
}

bool hda_present(void) { return g_ok; }

/* ---- playback bookkeeping ---- */

static uint32_t lpib(void) { return r32(g_sd + SD_LPIB) % RING_BYTES; }

/* advance by what the hardware played since last time: count it off the
   pending data and silence it, so an underrun plays zeros */
static void account(void)
{
    uint32_t pos = lpib();
    uint32_t played = (pos + RING_BYTES - g_last_lpib) % RING_BYTES;
    if (played) {
        uint32_t p = g_last_lpib;
        for (uint32_t i = 0; i < played; i++) { g_ring[p] = 0; p = (p + 1) % RING_BYTES; }
        g_last_lpib = pos;
        if (played >= g_pending) {
            g_pending = 0;                 /* all played (or an underrun) */
            g_idle = true;
        } else {
            g_pending -= played;
        }
    }
}

void hda_tick(void)
{
    if (g_ok && g_running) account();
}

void hda_dsp_open(void)
{
    if (!g_ok) return;
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    account();
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

int64_t hda_dsp_write(const void *buf, uint32_t len)
{
    if (!g_ok) return -5;   /* -EIO */
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t done = 0;
    uint64_t deadline = timer_get_ms() + 2000;
    while (done < len) {
        account();
        if (g_idle) {
            /* start just ahead of the hardware; the silence before the
               data counts as queued (GETODELAY, SYNC) */
            g_wpos = ((lpib() + WRITE_LEAD) % RING_BYTES) & ~3u;
            g_pending = WRITE_LEAD;
            g_idle = false;
        }
        uint32_t space = g_pending >= MAX_QUEUED ? 0 : MAX_QUEUED - g_pending;
        if (space == 0) {
            if (timer_get_ms() > deadline) break;           /* hardware stuck: give up */
            sched_sleep_ms(5);
            continue;
        }
        uint32_t n = len - done < space ? len - done : space;
        /* copy_from_user each time: the buffer was checked before we
           slept, and another thread may have unmapped it since -- a plain
           read here faulted in the kernel */
        uint32_t first = RING_BYTES - g_wpos < n ? RING_BYTES - g_wpos : n;
        if (copy_from_user(g_ring + g_wpos, src + done, first) != 0 ||
            (n > first && copy_from_user(g_ring, src + done + first, n - first) != 0))
            return done ? (int64_t)done : -14;              /* -EFAULT */
        g_wpos = (g_wpos + n) % RING_BYTES;
        g_pending += n;
        done += n;
        deadline = timer_get_ms() + 2000;
    }
    return done ? (int64_t)done : -5;
}

/* ---- OSS ioctls ---- */

#define SNDCTL_DSP_RESET       0x00005000
#define SNDCTL_DSP_SYNC        0x00005001
#define SNDCTL_DSP_SPEED       0xC0045002
#define SNDCTL_DSP_STEREO      0xC0045003
#define SNDCTL_DSP_GETBLKSIZE  0xC0045004
#define SNDCTL_DSP_SETFMT      0xC0045005
#define SNDCTL_DSP_CHANNELS    0xC0045006
#define SNDCTL_DSP_POST        0x00005008
#define SNDCTL_DSP_SETFRAGMENT 0xC004500A
#define SNDCTL_DSP_GETFMTS     0x8004500B
#define SNDCTL_DSP_GETOSPACE   0x8010500C
#define SNDCTL_DSP_NONBLOCK    0x0000500E
#define SNDCTL_DSP_GETCAPS     0x8004500F
#define SNDCTL_DSP_GETODELAY   0x80045017
#define AFMT_S16_LE            0x00000010
#define DSP_CAP_REALTIME       0x00000200
#define DSP_CAP_TRIGGER        0x00001000

static int64_t get_int(uint64_t argp, int *v)
{
    if (!argp || copy_from_user(v, (const void *)argp, sizeof(*v)) != 0) return -14;
    return 0;
}

static int64_t put_int(uint64_t argp, int v)
{
    if (!argp || copy_to_user((void *)argp, &v, sizeof(v)) != 0) return -14;
    return 0;
}

static void reconfigure(uint32_t rate, uint32_t ch)
{
    if (rate == g_rate && ch == g_channels) return;
    stream_stop();
    g_rate = rate;
    g_channels = ch;
    stream_start();
}

int64_t hda_dsp_ioctl(uint64_t request, uint64_t argp)
{
    if (!g_ok) return -5;
    int v = 0;
    switch (request) {
    case SNDCTL_DSP_RESET:
        stream_stop();
        stream_start();
        return 0;
    case SNDCTL_DSP_SYNC: {
        uint64_t deadline = timer_get_ms() + 2000;
        for (;;) {
            account();
            if (!g_pending || timer_get_ms() > deadline) break;
            sched_sleep_ms(5);
        }
        return 0;
    }
    case SNDCTL_DSP_POST:
    case SNDCTL_DSP_NONBLOCK:
        return 0;
    case SNDCTL_DSP_SPEED:
        if (get_int(argp, &v)) return -14;
        reconfigure(v < 46000 ? 44100 : 48000, g_channels);   /* the two the codec takes here */
        return put_int(argp, (int)g_rate);
    case SNDCTL_DSP_STEREO:
        if (get_int(argp, &v)) return -14;
        reconfigure(g_rate, v ? 2 : 1);
        return put_int(argp, g_channels == 2);
    case SNDCTL_DSP_CHANNELS:
        if (get_int(argp, &v)) return -14;
        reconfigure(g_rate, v <= 1 ? 1 : 2);
        return put_int(argp, (int)g_channels);
    case SNDCTL_DSP_SETFMT:
        if (get_int(argp, &v)) return -14;
        return put_int(argp, AFMT_S16_LE);                   /* the only one */
    case SNDCTL_DSP_GETFMTS:
        return put_int(argp, AFMT_S16_LE);
    case SNDCTL_DSP_GETCAPS:
        return put_int(argp, DSP_CAP_REALTIME | DSP_CAP_TRIGGER);
    case SNDCTL_DSP_GETBLKSIZE:
        return put_int(argp, 4096);
    case SNDCTL_DSP_SETFRAGMENT:
        return 0;                                            /* fixed 4 KB fragments */
    case SNDCTL_DSP_GETODELAY:
        account();
        return put_int(argp, (int)g_pending);
    case SNDCTL_DSP_GETOSPACE: {
        account();
        uint32_t space = g_pending >= MAX_QUEUED ? 0 : MAX_QUEUED - g_pending;
        int info[4] = { (int)(space / 4096), (int)(MAX_QUEUED / 4096), 4096, (int)space };
        if (!argp || copy_to_user((void *)argp, info, sizeof(info)) != 0) return -14;
        return 0;
    }
    }
    return -25;   /* -ENOTTY */
}
