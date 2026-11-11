/*
 * WynlandOS - wifid: WPA2-Personal for the Wi-Fi card.
 *
 * The kernel driver (drivers/net/iwx.c) moves frames and installs keys; it
 * never sees a password. This program holds it: it asks the card for the
 * networks around, joins one, and does the 4-way handshake (IEEE 802.11i)
 * with libcrypto -- PBKDF2 and HMAC-SHA1 for the keys and the MIC, AES key
 * unwrap for the group key -- then hands the pairwise and group keys back.
 * The card encrypts and decrypts by itself from there.
 *
 * It speaks to the driver through /dev/wifi (root's): a write is one
 * command, a read is one event, or 0 when there is none.
 *
 *   commands: 00 hello | 01 scan | 02 join (bssid[6] rsnlen rsn[]) |
 *             03 EAPOL frame | 04 key (kind id TK[16] PN[6]) | 05 leave
 *   events:   81 network | 82 scan done | 83 associated | 84 join failed |
 *             85 EAPOL frame | 86 link up | 87 link down | 88 our address
 *
 * The network and its password, for now: /etc/wynland/wifi.conf
 * ("ssid=" and "psk=" lines), else boot.cfg's "wifi_ssid=" / "wifi_psk=".
 * Its log lines go to the kernel log (/dev/kmsg). The password and the
 * keys are never logged.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>

static int g_dev = -1, g_kmsg = -1;

static void logf_(const char *fmt, ...)
{
    char b[300];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 3, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 3) n = (int)sizeof b - 3;
    memcpy(b + n, "\r\n", 3);
    if (g_kmsg >= 0) { if (write(g_kmsg, b, (size_t)n + 2) < 0) { } }
}

static void msleep(int ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static inline unsigned be16(const uint8_t *p) { return (unsigned)(p[0] << 8 | p[1]); }
static inline unsigned le16(const uint8_t *p) { return (unsigned)(p[0] | p[1] << 8); }
static inline void put_be16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* ---- the driver ------------------------------------------------------- */
static int dev_send(const uint8_t *b, int n) { return (int)write(g_dev, b, (size_t)n); }

/* the next event within ms milliseconds; its length, 0 for none */
static int ev_wait(uint8_t *b, int cap, int ms)
{
    for (int t = 0;; t += 5) {
        int n = (int)read(g_dev, b, (size_t)cap);
        if (n > 0) return n;
        if (t >= ms) return 0;
        msleep(5);
    }
}

/* ---- configuration ---------------------------------------------------- */
static char g_ssid[64], g_psk[96];

static void cfg_scan(FILE *f, const char *ks, const char *kp)
{
    char line[256];
    size_t ls = strlen(ks), lp = strlen(kp);
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!strncmp(line, ks, ls) && line[ls] == '=') snprintf(g_ssid, sizeof g_ssid, "%s", line + ls + 1);
        else if (!strncmp(line, kp, lp) && line[lp] == '=') snprintf(g_psk, sizeof g_psk, "%s", line + lp + 1);
    }
}

static bool load_config(void)
{
    FILE *f = fopen("/etc/wynland/wifi.conf", "r");
    if (f) { cfg_scan(f, "ssid", "psk"); fclose(f); }
    if (!g_ssid[0]) {
        f = fopen("/etc/wynland/boot.cfg", "r");
        if (f) { cfg_scan(f, "wifi_ssid", "wifi_psk"); fclose(f); }
    }
    return g_ssid[0] && strlen(g_psk) >= 8;
}

/* ---- keys: IEEE 802.11i ------------------------------------------------- */
static uint8_t g_pmk[32], g_ptk[48], g_snonce[32], g_anonce[32];
static uint8_t g_mac[6], g_ap[6], g_rsn[24];
static uint8_t g_last_rc[8];
static int g_hs;                    /* 0: nothing yet, 1: message 2 sent, 2: keys in */
static bool g_linkup;

/* PRF-SHA1 (802.11i 8.5.1.1) */
static void prf(const uint8_t *key, int klen, const char *label, const uint8_t *data, int dlen, uint8_t *out, int outlen)
{
    uint8_t buf[200];
    int ll = (int)strlen(label);
    memcpy(buf, label, (size_t)ll);
    buf[ll] = 0;
    memcpy(buf + ll + 1, data, (size_t)dlen);
    int len = ll + 1 + dlen;
    for (int i = 0; i * 20 < outlen; i++) {
        uint8_t h[20];
        unsigned hl = sizeof h;
        buf[len] = (uint8_t)i;
        HMAC(EVP_sha1(), key, klen, buf, (size_t)len + 1, h, &hl);
        int c = outlen - i * 20 < 20 ? outlen - i * 20 : 20;
        memcpy(out + i * 20, h, (size_t)c);
    }
}

static void derive_ptk(void)
{
    uint8_t d[76];
    const uint8_t *lo = memcmp(g_ap, g_mac, 6) < 0 ? g_ap : g_mac, *hi = lo == g_ap ? g_mac : g_ap;
    memcpy(d, lo, 6);
    memcpy(d + 6, hi, 6);
    const uint8_t *nlo = memcmp(g_anonce, g_snonce, 32) < 0 ? g_anonce : g_snonce, *nhi = nlo == g_anonce ? g_snonce : g_anonce;
    memcpy(d + 12, nlo, 32);
    memcpy(d + 44, nhi, 32);
    prf(g_pmk, 32, "Pairwise key expansion", d, sizeof d, g_ptk, 48);     /* KCK 16, KEK 16, TK 16 */
}

/* RFC 3394 AES key unwrap with a 128-bit KEK; out is inlen - 8 bytes */
static bool aes_unwrap(const uint8_t *kek, const uint8_t *in, int inlen, uint8_t *out)
{
    if (inlen < 24 || inlen % 8) return false;
    int n = inlen / 8 - 1;
    uint8_t a[8], r[256];
    if (n * 8 > (int)sizeof r) return false;
    memcpy(a, in, 8);
    memcpy(r, in + 8, (size_t)n * 8);
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    if (!c || EVP_DecryptInit_ex(c, EVP_aes_128_ecb(), NULL, kek, NULL) != 1) { EVP_CIPHER_CTX_free(c); return false; }
    EVP_CIPHER_CTX_set_padding(c, 0);
    for (int j = 5; j >= 0; j--)
        for (int i = n; i >= 1; i--) {
            uint8_t b[16], o[16];
            int ol = 0;
            uint64_t t = (uint64_t)n * (uint64_t)j + (uint64_t)i;
            memcpy(b, a, 8);
            for (int k = 0; k < 8; k++) b[7 - k] ^= (uint8_t)(t >> (8 * k));
            memcpy(b + 8, r + (i - 1) * 8, 8);
            if (EVP_DecryptUpdate(c, o, &ol, b, 16) != 1 || ol != 16) { EVP_CIPHER_CTX_free(c); return false; }
            memcpy(a, o, 8);
            memcpy(r + (i - 1) * 8, o + 8, 8);
        }
    EVP_CIPHER_CTX_free(c);
    static const uint8_t iv[8] = { 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6 };
    if (memcmp(a, iv, 8)) return false;
    memcpy(out, r, (size_t)n * 8);
    return true;
}

/* EAPOL-Key (RSN, descriptor version 2): 4-byte EAPOL header, then
   desc 1, info 2, key length 2, replay 8, nonce 32, IV 16, RSC 8, id 8,
   MIC 16, data length 2, data */
#define KEYHDR 99

static void mic_of(const uint8_t *kck, const uint8_t *f, int total, uint8_t *mic16)
{
    uint8_t c[2048], h[20];
    unsigned hl = sizeof h;
    memcpy(c, f, (size_t)total);
    memset(c + 81, 0, 16);
    HMAC(EVP_sha1(), kck, 16, c, (size_t)total, h, &hl);
    memcpy(mic16, h, 16);
}

static void send_key(unsigned info, const uint8_t *rc, const uint8_t *nonce, const uint8_t *kd, int kdlen)
{
    uint8_t req[1 + KEYHDR + 64];
    uint8_t *f = req + 1;
    int total = KEYHDR + kdlen;
    memset(req, 0, sizeof req);
    req[0] = 0x03;
    f[0] = 2; f[1] = 3;                                 /* EAPOL version, type Key */
    put_be16(f + 2, (unsigned)total - 4);
    f[4] = 2;                                           /* the RSN descriptor */
    put_be16(f + 5, info);
    memcpy(f + 9, rc, 8);
    if (nonce) memcpy(f + 17, nonce, 32);
    put_be16(f + 97, (unsigned)kdlen);
    if (kdlen) memcpy(f + KEYHDR, kd, (size_t)kdlen);
    if (info & 0x0100) mic_of(g_ptk, f, total, f + 81);
    dev_send(req, 1 + total);
}

static void send_key_ptk(unsigned info, const uint8_t *rc, const uint8_t *nonce, const uint8_t *kd, int kdlen)
{
    send_key(info, rc, nonce, kd, kdlen);               /* (the KCK is the first 16 bytes of g_ptk) */
}

static bool check_mic(const uint8_t *f, int n)
{
    int total = 4 + (int)be16(f + 2);
    if (total > n || total > 2000) return false;
    uint8_t m[16];
    mic_of(g_ptk, f, total, m);
    return memcmp(m, f + 81, 16) == 0;
}

static void install(uint8_t kind, uint8_t id, const uint8_t *tk)
{
    uint8_t c[25];
    memset(c, 0, sizeof c);
    c[0] = 0x04; c[1] = kind; c[2] = id;
    memcpy(c + 3, tk, 16);                              /* the PN field stays 0 */
    dev_send(c, sizeof c);
}

/* the GTK in the key data: a vendor element 00-0f-ac:1, key info, reserved, key */
static bool find_gtk(const uint8_t *d, int n, uint8_t *id, uint8_t *gtk)
{
    for (int i = 0; i + 2 <= n;) {
        int el = d[i], l = d[i + 1];
        if (el == 0xdd && l == 0) break;                /* the padding */
        if (i + 2 + l > n) break;
        if (el == 0xdd && l >= 6 + 16 && d[i + 2] == 0 && d[i + 3] == 0x0f && d[i + 4] == 0xac && d[i + 5] == 1) {
            *id = d[i + 6] & 3;
            memcpy(gtk, d + i + 8, 16);
            return true;
        }
        i += 2 + l;
    }
    return false;
}

static void handle_key(const uint8_t *f, int n)
{
    if (n < KEYHDR || f[1] != 3 || (f[4] != 2 && f[4] != 254)) return;
    unsigned info = be16(f + 5);
    int kdlen = (int)be16(f + 97);
    if (KEYHDR + kdlen > n) return;
    if ((info & 7) != 2) { logf_("wifid: EAPOL-Key descriptor version %u is not supported", info & 7); return; }
    bool pairwise = info & 0x0008, ack = info & 0x0080, mic = info & 0x0100, install_ = info & 0x0040, enc = info & 0x1000;
    const uint8_t *rc = f + 9;

    if (pairwise && ack && !mic) {                      /* message 1: ANonce -> PTK, message 2 */
        memcpy(g_anonce, f + 17, 32);
        int rf = open("/dev/urandom", O_RDONLY);
        if (rf < 0 || read(rf, g_snonce, 32) != 32) { logf_("wifid: no random numbers"); if (rf >= 0) close(rf); return; }
        close(rf);
        derive_ptk();
        memcpy(g_last_rc, rc, 8);
        g_hs = 1;
        send_key_ptk(0x010a, rc, g_snonce, g_rsn, 22);
        logf_("wifid: handshake message 1 -> 2");
        return;
    }
    if (g_hs < 1) return;
    if (pairwise && ack && mic && install_) {           /* message 3: the keys, message 4 */
        if (memcmp(f + 17, g_anonce, 32)) { logf_("wifid: message 3 with another ANonce"); return; }
        if (memcmp(rc, g_last_rc, 8) <= 0 && g_hs == 2) { return; }      /* a replay of the last one */
        if (!check_mic(f, n)) { logf_("wifid: message 3: WRONG MIC (the password?)"); return; }
        uint8_t kd[256], gtk[16], gid = 0;
        int kl = kdlen;
        if (enc) {
            if (kdlen > 264 || !aes_unwrap(g_ptk + 16, f + KEYHDR, kdlen, kd)) { logf_("wifid: message 3: key data did not unwrap"); return; }
            kl = kdlen - 8;
        } else if (kdlen <= (int)sizeof kd) memcpy(kd, f + KEYHDR, (size_t)kdlen);
        if (!find_gtk(kd, kl, &gid, gtk)) { logf_("wifid: message 3 carries no group key"); return; }
        memcpy(g_last_rc, rc, 8);
        send_key_ptk(0x030a, rc, NULL, NULL, 0);
        logf_("wifid: handshake message 3 -> 4");
        install(0, 0, g_ptk + 32);
        install(1, gid, gtk);
        g_hs = 2;
        memset(gtk, 0, sizeof gtk);
        return;
    }
    if (!pairwise && ack && mic && g_hs == 2) {         /* the group key handshake: a new GTK */
        if (memcmp(rc, g_last_rc, 8) <= 0) return;
        if (!check_mic(f, n)) { logf_("wifid: group message with a wrong MIC"); return; }
        uint8_t kd[256], gtk[16], gid = 0;
        if (!enc || kdlen > 264 || !aes_unwrap(g_ptk + 16, f + KEYHDR, kdlen, kd) || !find_gtk(kd, kdlen - 8, &gid, gtk)) {
            logf_("wifid: group message: no usable key"); return;
        }
        memcpy(g_last_rc, rc, 8);
        send_key_ptk(0x0302, rc, NULL, NULL, 0);
        install(1, gid, gtk);
        logf_("wifid: group key renewed");
        memset(gtk, 0, sizeof gtk);
    }
}

/* ---- the network ------------------------------------------------------------ */
typedef struct { uint8_t bssid[6]; uint8_t ch; int rssi; uint8_t rsn[64]; int rsnlen; } Net;

/* our RSN element from the AP's: CCMP, PSK, no management-frame protection */
static bool make_rsn(const Net *n)
{
    const uint8_t *e = n->rsn;
    if (n->rsnlen < 22 || e[0] != 48) { logf_("wifid: the network has no WPA2 (RSN) element"); return false; }
    static const uint8_t ccmp[4] = { 0x00, 0x0f, 0xac, 0x04 };
    if (memcmp(e + 4, ccmp, 4)) { logf_("wifid: the group cipher is not CCMP (TKIP is not supported)"); return false; }
    int np = (int)le16(e + 8), o = 10 + 4 * np;
    bool pc = false, psk = false;
    for (int i = 0; i < np && 10 + 4 * i + 4 <= n->rsnlen; i++) if (!memcmp(e + 10 + 4 * i, ccmp, 4)) pc = true;
    if (o + 2 > n->rsnlen) return false;
    int na = (int)le16(e + o);
    for (int i = 0; i < na && o + 2 + 4 * i + 4 <= n->rsnlen; i++) {
        const uint8_t *a = e + o + 2 + 4 * i;
        if (a[0] == 0 && a[1] == 0x0f && a[2] == 0xac && a[3] == 2) psk = true;
    }
    if (!pc) { logf_("wifid: the network does not offer CCMP"); return false; }
    if (!psk) { logf_("wifid: the network does not offer WPA2-PSK (SAE / enterprise only)"); return false; }
    uint8_t r[22] = { 0x30, 0x14, 0x01, 0x00, 0, 0, 0, 0, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
                      0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00 };
    memcpy(r + 4, e + 4, 4);
    memcpy(g_rsn, r, sizeof r);
    return true;
}

/* scan; the strongest network of that name */
static bool find_network(Net *out)
{
    uint8_t cmd = 0x01, e[512];
    dev_send(&cmd, 1);
    bool found = false;
    for (uint64_t until = now_ms() + 40000; now_ms() < until;) {
        int n = ev_wait(e, sizeof e, 500);
        if (n <= 0) continue;
        if (e[0] == 0x82) break;
        if (e[0] != 0x81 || n < 11) continue;
        int sl = e[9];
        if (10 + sl + 1 > n) continue;
        if (sl == (int)strlen(g_ssid) && !memcmp(e + 10, g_ssid, (size_t)sl) && !found) {
            memcpy(out->bssid, e + 1, 6);
            out->ch = e[7];
            out->rssi = (int8_t)e[8];
            out->rsnlen = e[10 + sl];
            if (10 + sl + 1 + out->rsnlen > n || out->rsnlen > 64) continue;
            memcpy(out->rsn, e + 10 + sl + 1, (size_t)out->rsnlen);
            found = true;                               /* (they come strongest first) */
        }
    }
    return found;
}

/* one go: scan, join, handshake, then watch the link; returns when it is over */
static void session(void)
{
    Net net;
    if (!find_network(&net)) { logf_("wifid: network \"%s\" not seen", g_ssid); return; }
    logf_("wifid: found \"%s\": channel %u, %d dBm", g_ssid, net.ch, net.rssi);
    if (!make_rsn(&net)) return;
    if (!PKCS5_PBKDF2_HMAC_SHA1(g_psk, (int)strlen(g_psk), (const unsigned char *)g_ssid, (int)strlen(g_ssid), 4096, 32, g_pmk)) {
        logf_("wifid: PBKDF2 failed");
        return;
    }
    memcpy(g_ap, net.bssid, 6);
    uint8_t j[1 + 6 + 1 + 22];
    j[0] = 0x02;
    memcpy(j + 1, net.bssid, 6);
    j[7] = 22;
    memcpy(j + 8, g_rsn, 22);
    dev_send(j, sizeof j);
    g_hs = 0;
    g_linkup = false;

    uint8_t e[1800];
    bool assoc = false;
    uint64_t until = now_ms() + 12000;                  /* association, then the handshake */
    while (now_ms() < until || g_linkup) {
        int n = ev_wait(e, sizeof e, 200);
        if (n <= 0) continue;
        switch (e[0]) {
        case 0x83:
            assoc = true;
            logf_("wifid: associated; waiting for the handshake");
            until = now_ms() + 12000;
            break;
        case 0x84:
            logf_("wifid: could not join: stage %u, status %u", le16(e + 1), le16(e + 3));
            return;
        case 0x85:
            if (n > 7) handle_key(e + 7, n - 7);
            break;
        case 0x86:
            g_linkup = true;
            logf_("wifid: LINK UP -- WPA2 handshake done");
            break;
        case 0x87:
            logf_("wifid: link down, reason %u", le16(e + 1));
            return;
        }
    }
    uint8_t leave = 0x05;
    dev_send(&leave, 1);
    logf_("wifid: %s", assoc ? "the handshake did not finish" : "no association");
}

int main(void)
{
    g_kmsg = open("/dev/kmsg", O_WRONLY);
    logf_("wifid: started");
    if (!load_config()) {
        logf_("wifid: no network configured (ssid / psk of at least 8 characters): nothing to do");
        for (;;) sleep(3600);
    }
    while ((g_dev = open("/dev/wifi", O_RDWR)) < 0) msleep(1000);       /* (the card comes up a few seconds after boot) */
    uint8_t hello = 0x00, e[64];
    dev_send(&hello, 1);
    for (int i = 0; i < 100; i++) {
        int n = ev_wait(e, sizeof e, 100);
        if (n >= 7 && e[0] == 0x88) { memcpy(g_mac, e + 1, 6); break; }
    }
    logf_("wifid: the card is %02x:%02x:%02x:%02x:%02x:%02x", g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    for (;;) {
        session();
        msleep(5000);
    }
}
