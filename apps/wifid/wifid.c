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
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

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
static int g_hs, g_hs_msg1;                    /* 0: nothing yet, 1: message 2 sent, 2: keys in */
static bool g_linkup, g_wrong_mic;

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
        g_wrong_mic = g_hs_msg1++ > 0;               /* message 1 again: our message 2 was refused -- the password */
        logf_("wifid: handshake message 1 -> 2");
        return;
    }
    if (g_hs < 1) return;
    if (pairwise && ack && mic && install_) {           /* message 3: the keys, message 4 */
        if (memcmp(f + 17, g_anonce, 32)) { logf_("wifid: message 3 with another ANonce"); return; }
        if (memcmp(rc, g_last_rc, 8) <= 0 && g_hs == 2) { return; }      /* a replay of the last one */
        if (!check_mic(f, n)) { logf_("wifid: message 3: WRONG MIC (the password?)"); g_wrong_mic = true; return; }
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

/* ---- the networks seen, the networks known ---------------------------------- */
#define MAX_SEEN  64
#define MAX_KNOWN 32
typedef struct { char ssid[33]; Net n; char sec[12]; } Seen;
typedef struct { char ssid[33]; char psk[64]; bool saved; bool bad; } Known;
static Seen g_seen[MAX_SEEN], g_fresh[MAX_SEEN];  /* the last scan's; the one coming in */
static int g_nseen, g_nfresh;
static bool g_bgscan;                           /* a scan while connected */
static uint64_t g_bg_deadline;
static Known g_known[MAX_KNOWN];
static int g_nknown;
#define KNOWN_FILE "/var/lib/wifid/networks"

static const char *sec_of(const uint8_t *rsn, int len)
{
    if (len < 2) return "open";
    bool psk = false, sae = false;
    if (len >= 10) {
        int np = (int)le16(rsn + 8), o = 10 + 4 * np;
        if (o + 2 <= len) {
            int na = (int)le16(rsn + o);
            for (int i = 0; i < na && o + 2 + 4 * i + 4 <= len; i++) {
                const uint8_t *a = rsn + o + 2 + 4 * i;
                if (a[0] == 0 && a[1] == 0x0f && a[2] == 0xac) { if (a[3] == 2) psk = true; if (a[3] == 8) sae = true; }
            }
        }
    }
    return psk ? "wpa2" : sae ? "wpa3" : "other";
}

static Known *known_find(const char *ssid)
{
    for (int i = 0; i < g_nknown; i++) if (!strcmp(g_known[i].ssid, ssid)) return &g_known[i];
    return NULL;
}

static void known_save(void)
{
    mkdir("/var/lib/wifid", 0700);
    FILE *f = fopen(KNOWN_FILE ".new", "w");
    if (!f) { logf_("wifid: cannot write %s", KNOWN_FILE); return; }
    fchmod(fileno(f), 0600);
    for (int i = 0; i < g_nknown; i++)
        if (g_known[i].saved) fprintf(f, "%s\t%s\n", g_known[i].ssid, g_known[i].psk);
    fclose(f);
    rename(KNOWN_FILE ".new", KNOWN_FILE);
}

static Known *known_set(const char *ssid, const char *psk, bool saved)
{
    Known *k = known_find(ssid);
    if (!k) {
        if (g_nknown >= MAX_KNOWN) return NULL;
        k = &g_known[g_nknown++];
        memset(k, 0, sizeof *k);
        snprintf(k->ssid, sizeof k->ssid, "%s", ssid);
    }
    snprintf(k->psk, sizeof k->psk, "%s", psk);
    k->bad = false;
    if (saved) k->saved = true;
    return k;
}

static void known_forget(const char *ssid)
{
    for (int i = 0; i < g_nknown; i++)
        if (!strcmp(g_known[i].ssid, ssid)) {
            memset(g_known[i].psk, 0, sizeof g_known[i].psk);
            g_known[i] = g_known[--g_nknown];
            break;
        }
    known_save();
}

/* the saved ones, then the configuration files' (not written back: boot.cfg stays the source) */
static void known_load(void)
{
    FILE *f = fopen(KNOWN_FILE, "r");
    char line[200];
    if (f) {
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            char *t = strchr(line, '\t');
            if (!t) continue;
            *t = 0;
            if (line[0] && strlen(t + 1) >= 8) known_set(line, t + 1, true);
        }
        fclose(f);
    }
    if (load_config() && !known_find(g_ssid)) known_set(g_ssid, g_psk, false);
    memset(g_psk, 0, sizeof g_psk);
}

/* ---- the state ------------------------------------------------------------------ */
enum { ST_IDLE, ST_SCANNING, ST_JOINING, ST_HANDSHAKE, ST_CONNECTED };
static const char *st_name[] = { "idle", "scanning", "connecting", "connecting", "connected" };
static int g_state = ST_IDLE;
static uint64_t g_deadline, g_next_scan;
static bool g_auto = true;                      /* off after "disconnect", on again with "connect" */
static char g_cur[33], g_want[33], g_reason[80];
static int g_cur_rssi, g_cur_ch;

static void scan_start(void)
{
    uint8_t c = 0x01;
    g_nfresh = 0;
    dev_send(&c, 1);
    g_state = ST_SCANNING;
    g_deadline = now_ms() + 40000;
}

static void join(Seen *s, Known *k)
{
    if (!make_rsn(&s->n)) { snprintf(g_reason, sizeof g_reason, "unsupported security"); g_state = ST_IDLE; return; }
    if (!PKCS5_PBKDF2_HMAC_SHA1(k->psk, (int)strlen(k->psk), (const unsigned char *)s->ssid, (int)strlen(s->ssid), 4096, 32, g_pmk)) {
        g_state = ST_IDLE;
        return;
    }
    snprintf(g_cur, sizeof g_cur, "%s", s->ssid);
    g_cur_rssi = s->n.rssi;
    g_cur_ch = s->n.ch;
    memcpy(g_ap, s->n.bssid, 6);
    uint8_t j[1 + 6 + 1 + 22];
    j[0] = 0x02;
    memcpy(j + 1, s->n.bssid, 6);
    j[7] = 22;
    memcpy(j + 8, g_rsn, 22);
    g_hs = 0;
    g_hs_msg1 = 0;
    g_wrong_mic = false;
    g_linkup = false;
    dev_send(j, sizeof j);
    g_state = ST_JOINING;
    g_deadline = now_ms() + 12000;
    g_reason[0] = 0;
    logf_("wifid: joining \"%s\" (channel %u, %d dBm)", s->ssid, s->n.ch, s->n.rssi);
}

/* after a scan: the wanted network, else the best known one (5 GHz preferred a little) */
static void pick(void)
{
    Seen *best = NULL;
    Known *bk = NULL;
    int bs = -1000;
    for (int i = 0; i < g_nseen; i++) {
        Seen *s = &g_seen[i];
        Known *k = known_find(s->ssid);
        if (!k || k->bad || strcmp(s->sec, "wpa2")) continue;
        if (g_want[0] && strcmp(g_want, s->ssid)) continue;
        int score = s->n.rssi + (s->n.ch > 14 && s->n.rssi > -72 ? 12 : 0);
        if (score > bs) { bs = score; best = s; bk = k; }
    }
    if (best) { join(best, bk); return; }
    if (g_want[0]) snprintf(g_reason, sizeof g_reason, "network not found");
    g_want[0] = 0;
    g_state = ST_IDLE;
    g_next_scan = now_ms() + 20000;
}

static void leave(void)
{
    uint8_t c = 0x05;
    if (g_state >= ST_JOINING) dev_send(&c, 1);
    g_state = ST_IDLE;
    g_cur[0] = 0;
    g_linkup = false;
}

static void on_event(const uint8_t *e, int n)
{
    switch (e[0]) {
    case 0x81: {                                        /* a network */
        if (n < 11 || g_nfresh >= MAX_SEEN) break;
        int sl = e[9];
        if (10 + sl + 1 > n || sl == 0) break;
        int rl = e[10 + sl];
        if (10 + sl + 1 + rl > n || rl > 64) break;
        char ssid[33];
        memcpy(ssid, e + 10, (size_t)sl);
        ssid[sl] = 0;
        for (int i = 0; i < g_nfresh; i++) if (!strcmp(g_fresh[i].ssid, ssid)) { sl = -1; break; }   /* (strongest first) */
        if (sl < 0) break;
        Seen *s = &g_fresh[g_nfresh++];
        snprintf(s->ssid, sizeof s->ssid, "%s", ssid);
        memcpy(s->n.bssid, e + 1, 6);
        s->n.ch = e[7];
        s->n.rssi = (int8_t)e[8];
        s->n.rsnlen = rl;
        memcpy(s->n.rsn, e + 11 + e[9], (size_t)rl);
        snprintf(s->sec, sizeof s->sec, "%s", sec_of(s->n.rsn, rl));
        break;
    }
    case 0x82:                                          /* the scan is over */
        memcpy(g_seen, g_fresh, sizeof(Seen) * (size_t)g_nfresh);   /* its list replaces the last */
        g_nseen = g_nfresh;
        g_nfresh = 0;
        g_bgscan = false;
        if (g_state == ST_SCANNING) pick();
        break;
    case 0x83:
        if (g_state == ST_JOINING) { g_state = ST_HANDSHAKE; g_deadline = now_ms() + 12000; }
        logf_("wifid: associated; waiting for the handshake");
        break;
    case 0x84:
        logf_("wifid: could not join: stage %u, status %u", le16(e + 1), le16(e + 3));
        snprintf(g_reason, sizeof g_reason, "the access point refused us");
        g_state = ST_IDLE;
        g_next_scan = now_ms() + 10000;
        break;
    case 0x85:
        if (n > 7) handle_key(e + 7, n - 7);
        break;
    case 0x86:
        g_linkup = true;
        g_state = ST_CONNECTED;
        g_want[0] = 0;
        logf_("wifid: LINK UP -- WPA2 handshake done");
        break;
    case 0x87:
        logf_("wifid: link down, reason %u", le16(e + 1));
        if (g_state < ST_JOINING) break;                /* (the answer to our own "leave") */
        if (g_state >= ST_JOINING) snprintf(g_reason, sizeof g_reason, "link lost (reason %u)", le16(e + 1));
        g_state = ST_IDLE;
        g_cur[0] = 0;
        g_linkup = false;
        g_next_scan = now_ms() + 3000;
        break;
    }
}

static void tick(void)
{
    uint64_t t = now_ms();
    if ((g_state == ST_JOINING || g_state == ST_HANDSHAKE) && t > g_deadline) {
        Known *k = known_find(g_cur);
        if (g_wrong_mic && k) {
            k->bad = true;
            snprintf(g_reason, sizeof g_reason, "wrong password");
            logf_("wifid: \"%s\": the password is wrong -- not trying it again until it is changed", g_cur);
        } else snprintf(g_reason, sizeof g_reason, "%s", g_state == ST_JOINING ? "no association" : "the handshake did not finish");
        leave();
        g_next_scan = t + 10000;
    }
    if (g_state == ST_SCANNING && t > g_deadline) pick();
    if (g_bgscan && t > g_bg_deadline) g_bgscan = false;
    if (g_state == ST_IDLE && g_auto && t >= g_next_scan) {
        bool any = false;
        for (int i = 0; i < g_nknown; i++) if (!g_known[i].bad) any = true;
        if (any || g_want[0]) scan_start();
        else g_next_scan = t + 30000;
    }
}

/* ---- the control socket (abstract "wifid"): the shell's and the wifi command's ---------- */
#define MAX_CLI 8
typedef struct { int fd; char in[512]; int n; } Cli;
static Cli g_cli[MAX_CLI];
static int g_lsock = -1;

static void out(int fd, const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) { if (send(fd, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1), MSG_NOSIGNAL) < 0) { } }
}

/* one request: "status", "list", "scan", "connect SSID<TAB>PASSWORD", "forget SSID", "disconnect" */
static void request(int fd, char *l)
{
    if (!strcmp(l, "status")) {
        out(fd, "state\t%s\n", st_name[g_state]);
        out(fd, "scanning\t%d\n", g_state == ST_SCANNING || g_bgscan ? 1 : 0);
        if (g_cur[0]) out(fd, "ssid\t%s\nsignal\t%d\nchannel\t%d\n", g_cur, g_cur_rssi, g_cur_ch);
        if (g_reason[0]) out(fd, "reason\t%s\n", g_reason);
        out(fd, "auto\t%d\n", g_auto ? 1 : 0);
    } else if (!strcmp(l, "list")) {
        for (int i = 0; i < g_nseen; i++) {
            Known *k = known_find(g_seen[i].ssid);
            out(fd, "net\t%s\t%d\t%u\t%s\t%d\t%d\n", g_seen[i].ssid, g_seen[i].n.rssi, g_seen[i].n.ch, g_seen[i].sec,
                k ? (k->bad ? 2 : 1) : 0, (g_state == ST_CONNECTED && !strcmp(g_cur, g_seen[i].ssid)) ? 1 : 0);
        }
    } else if (!strcmp(l, "scan")) {
        if (g_state == ST_IDLE) scan_start();
        else if (g_state == ST_CONNECTED && !g_bgscan) {        /* connected: the card scans in the background */
            uint8_t c = 0x01;
            g_nfresh = 0;
            g_bgscan = true;
            g_bg_deadline = now_ms() + 40000;
            dev_send(&c, 1);
        }
        out(fd, "ok\n");
    } else if (!strncmp(l, "connect ", 8)) {
        char *ssid = l + 8, *pw = strchr(ssid, '\t');
        if (pw) *pw++ = 0;
        if (!ssid[0] || strlen(ssid) > 32) { out(fd, "error\tbad network name\n"); goto end; }
        Known *k = known_find(ssid);
        if (pw && pw[0]) {
            size_t pl = strlen(pw);
            if (pl < 8 || pl > 63) { out(fd, "error\tthe password must be 8 to 63 characters\n"); goto end; }
            k = known_set(ssid, pw, true);
            known_save();
        }
        if (!k) { out(fd, "error\tneed a password\n"); goto end; }
        k->bad = false;
        snprintf(g_want, sizeof g_want, "%s", ssid);
        g_auto = true;
        g_reason[0] = 0;
        leave();
        scan_start();
        out(fd, "ok\n");
    } else if (!strncmp(l, "forget ", 7)) {
        if (!strcmp(g_cur, l + 7)) leave();
        known_forget(l + 7);
        out(fd, "ok\n");
    } else if (!strcmp(l, "enable")) {                          /* Wi-Fi on: automatic joining again */
        g_auto = true;
        g_next_scan = 0;
        out(fd, "ok\n");
    } else if (!strcmp(l, "disconnect")) {
        g_auto = false;
        leave();
        out(fd, "ok\n");
    } else out(fd, "error\tunknown request\n");
end:
    out(fd, ".\n");
}

static void serve(void)
{
    struct pollfd p[1 + MAX_CLI];
    int np = 0;
    p[np++] = (struct pollfd){ g_lsock, POLLIN, 0 };
    for (int i = 0; i < MAX_CLI; i++) if (g_cli[i].fd >= 0) p[np++] = (struct pollfd){ g_cli[i].fd, POLLIN, 0 };
    if (poll(p, (nfds_t)np, 20) <= 0) return;
    if (p[0].revents & POLLIN) {
        int c = accept4(g_lsock, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (c >= 0) {
            int i;
            for (i = 0; i < MAX_CLI && g_cli[i].fd >= 0; i++) { }
            if (i == MAX_CLI) close(c);
            else { g_cli[i].fd = c; g_cli[i].n = 0; }
        }
    }
    for (int i = 0; i < MAX_CLI; i++) {
        Cli *c = &g_cli[i];
        if (c->fd < 0) continue;
        ssize_t r = recv(c->fd, c->in + c->n, sizeof c->in - 1 - (size_t)c->n, 0);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { close(c->fd); c->fd = -1; continue; }
        if (r < 0) continue;
        c->n += (int)r;
        char *nl;
        while ((nl = memchr(c->in, '\n', (size_t)c->n))) {
            *nl = 0;
            if (nl > c->in && nl[-1] == '\r') nl[-1] = 0;
            request(c->fd, c->in);
            int used = (int)(nl - c->in) + 1;
            memmove(c->in, nl + 1, (size_t)(c->n - used));
            c->n -= used;
        }
        if (c->n >= (int)sizeof c->in - 1) c->n = 0;
        memset(c->in + c->n, 0, sizeof c->in - (size_t)c->n);   /* (a password never lingers) */
    }
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    g_kmsg = open("/dev/kmsg", O_WRONLY);
    logf_("wifid: started");
    for (int i = 0; i < MAX_CLI; i++) g_cli[i].fd = -1;
    known_load();
    logf_("wifid: %d known network(s)", g_nknown);

    g_lsock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    memcpy(a.sun_path, "\0wifid", 6);
    if (g_lsock < 0 || bind(g_lsock, (struct sockaddr *)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 6)) < 0 ||
        listen(g_lsock, 4) < 0) logf_("wifid: no control socket (%s)", strerror(errno));

    while ((g_dev = open("/dev/wifi", O_RDWR)) < 0) { serve(); msleep(500); }   /* (the card comes up a few seconds after boot) */
    uint8_t hello = 0x00, e[1800];
    dev_send(&hello, 1);
    for (int i = 0; i < 100; i++) {
        int n = ev_wait(e, sizeof e, 100);
        if (n >= 7 && e[0] == 0x88) { memcpy(g_mac, e + 1, 6); break; }
    }
    logf_("wifid: the card is %02x:%02x:%02x:%02x:%02x:%02x", g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    for (;;) {
        int n;
        while ((n = (int)read(g_dev, e, sizeof e)) > 0) on_event(e, n);
        tick();
        serve();
    }
}
