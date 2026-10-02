/*
 * WynlandOS - kernel random numbers.
 *
 * getrandom() used to hand out raw TSC readings and /dev/urandom zeros:
 * everything that made keys from them -- TLS session keys in curl's
 * LibreSSL, glibc's stack guard, hash seeds -- was predictable.
 *
 * A hash DRBG over SHA-256: out = H(key || counter || fresh), and after
 * every request the key is replaced by H(key || "rekey" || ...), so
 * output already handed out can't be recomputed from a later state.
 * "fresh" mixes in the CPU's hardware generator (RDSEED/RDRAND, present
 * on every x86-64 CPU of the last decade and in QEMU with -cpu host) and
 * the TSC on every call, so the state keeps gaining entropy.
 */
#include <wynland/random.h>
#include <wynland/sha256.h>
#include <wynland/rtc.h>

static uint8_t  g_key[32];
static uint64_t g_counter;
static bool     g_seeded;
static int      g_hw = -1;   /* -1 unknown, 0 none, 1 rdrand, 2 rdseed+rdrand */

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void detect_hw(void)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    int rdrand = (c >> 30) & 1;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    int rdseed = (b >> 18) & 1;
    g_hw = rdseed && rdrand ? 2 : rdrand ? 1 : 0;
}

/* one 64-bit word from the hardware generator; false if it has none or
   it keeps failing (it may, briefly, under load) */
static bool hw_word(uint64_t *out, bool seed)
{
    if (g_hw < 0) detect_hw();
    if (g_hw == 0 || (seed && g_hw < 2)) return false;
    for (int tries = 0; tries < 16; tries++) {
        uint64_t v;
        uint8_t ok;
        if (seed) __asm__ volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok));
        else      __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) { *out = v; return true; }
    }
    return false;
}

/* fresh input for this call: hardware words + TSC */
static void fresh(Sha256 *c)
{
    uint64_t w[5];
    for (int i = 0; i < 4; i++) {
        if (!hw_word(&w[i], false)) w[i] = 0;
    }
    w[4] = rdtsc();
    sha256_update(c, w, sizeof(w));
}

static void seed(void)
{
    Sha256 c;
    sha256_init(&c);
    uint64_t w[16];
    for (int i = 0; i < 16; i++) {
        if (!hw_word(&w[i], true) && !hw_word(&w[i], false)) w[i] = 0;
    }
    sha256_update(&c, w, sizeof(w));
    /* without a hardware generator: timing jitter across a few hash
       rounds and the clock -- weak, but no longer a constant */
    for (int i = 0; i < 64; i++) {
        uint64_t t = rdtsc();
        sha256_update(&c, &t, sizeof(t));
    }
    uint64_t now = rtc_get_unix_time();
    sha256_update(&c, &now, sizeof(now));
    sha256_final(&c, g_key);
    g_seeded = true;
}

void random_bytes(void *buf, uint64_t len)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");

    if (!g_seeded) seed();
    uint8_t *out = (uint8_t *)buf;
    uint8_t block[32];
    while (len > 0) {
        Sha256 c;
        sha256_init(&c);
        sha256_update(&c, g_key, sizeof(g_key));
        sha256_update(&c, &g_counter, sizeof(g_counter));
        fresh(&c);
        sha256_final(&c, block);
        g_counter++;
        uint64_t n = len < sizeof(block) ? len : sizeof(block);
        for (uint64_t i = 0; i < n; i++) out[i] = block[i];
        out += n;
        len -= n;
    }
    /* rekey: what was returned can't be recomputed from the new state */
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, g_key, sizeof(g_key));
    sha256_update(&c, "rekey", 5);
    sha256_update(&c, &g_counter, sizeof(g_counter));
    fresh(&c);
    sha256_final(&c, g_key);
    for (int i = 0; i < 32; i++) block[i] = 0;

    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
}

uint32_t random_u32(void)
{
    uint32_t v;
    random_bytes(&v, sizeof(v));
    return v;
}
