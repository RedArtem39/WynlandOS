/*
 * WynlandOS - the root password (`ary`).
 *
 * /etc/shadow, root-only (0600), one line:  root:<salt hex>:<hash hex>
 * hash = SHA-256 iterated AUTH_ROUNDS times over salt+password (salted,
 * deliberately slow). No file / no root line = no password set yet: the
 * first `ary login` creates it. The kernel reads and writes the file
 * itself (callers are unprivileged), via the root override of the VFS.
 */
#include <wynland/auth.h>
#include <wynland/sha256.h>
#include <wynland/rtc.h>
#include <wynland/types.h>
#include <wynland/vfs.h>

#define AUTH_FILE   "/etc/shadow"
#define AUTH_ROUNDS 20000

extern int g_vfs_root_override;   /* drivers/fs/ext2.c */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void to_hex(const uint8_t *in, int n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2 * i] = d[in[i] >> 4]; out[2 * i + 1] = d[in[i] & 15]; }
    out[2 * n] = 0;
}

static void derive(const uint8_t salt[8], const char *pw, uint8_t out[32])
{
    uint32_t len = 0;
    while (pw[len]) len++;
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, salt, 8);
    sha256_update(&c, pw, len);
    sha256_final(&c, out);
    for (int i = 1; i < AUTH_ROUNDS; i++) {
        sha256_init(&c);
        sha256_update(&c, out, 32);
        sha256_update(&c, salt, 8);
        sha256_update(&c, pw, len);
        sha256_final(&c, out);
    }
}

/* salt[8], hash[32] from the root line; false when there is none */
static bool read_root(uint8_t salt[8], uint8_t hash[32])
{
    VfsFile *f = vfs_open(AUTH_FILE);
    if (!f) return false;
    char line[128] = {0};
    int n = vfs_read(f, line, sizeof(line) - 1);
    vfs_close(f);
    if (n < 5 + 16 + 1 + 64 || line[0] != 'r' || line[1] != 'o' || line[2] != 'o' || line[3] != 't' || line[4] != ':')
        return false;
    const char *p = line + 5;
    for (int i = 0; i < 8; i++) {
        int hi = hexval(p[2 * i]), lo = hexval(p[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        salt[i] = (uint8_t)(hi << 4 | lo);
    }
    if (p[16] != ':') return false;
    p += 17;
    for (int i = 0; i < 32; i++) {
        int hi = hexval(p[2 * i]), lo = hexval(p[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        hash[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

bool auth_root_password_set(void)
{
    uint8_t s[8], h[32];
    return read_root(s, h);
}

bool auth_check_root(const char *pw)
{
    uint8_t salt[8], want[32], got[32];
    if (!read_root(salt, want)) return false;
    derive(salt, pw, got);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(want[i] ^ got[i]);   /* no early exit */
    return diff == 0;
}

bool auth_set_root(const char *pw)
{
    uint8_t salt[8], hash[32];
    uint64_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t seed = (hi << 32 | lo) ^ (rtc_get_unix_time() * 0x9E3779B97F4A7C15ULL);
    for (int i = 0; i < 8; i++) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; salt[i] = (uint8_t)seed; }
    derive(salt, pw, hash);

    char line[5 + 16 + 1 + 64 + 2];
    for (int i = 0; i < 5; i++) line[i] = "root:"[i];
    to_hex(salt, 8, line + 5);
    line[21] = ':';
    to_hex(hash, 32, line + 22);
    line[86] = '\n';
    line[87] = 0;

    g_vfs_root_override = 1;        /* the kernel writes it, as root */
    VfsStat st;
    if (!vfs_stat(AUTH_FILE, &st)) vfs_create(AUTH_FILE);
    VfsFile *f = vfs_open_flags(AUTH_FILE, VFS_O_WRITE | VFS_O_TRUNC);
    bool ok = false;
    if (f) { ok = vfs_write(f, line, 87) == 87; vfs_close(f); }
    vfs_chmod(AUTH_FILE, 0600);
    g_vfs_root_override = 0;
    return ok;
}
