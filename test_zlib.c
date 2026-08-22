/*
 * WynlandOS - zlib port verification.
 * Compresses a known string with compress(), decompresses it back with
 * uncompress(), and confirms the round-trip matches byte-for-byte -- real
 * proof zlib's deflate/inflate actually works on this OS, not just that it
 * links. Statically linked, no other dependencies.
 * Build:
 *   x86_64-linux-musl-gcc -static -O2 -Ibuild/zlib_headers/include \
 *     -o test_zlib.elf test_zlib.c build/lib/libz.a
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static const char *msg = "WynlandOS zlib round-trip test -- Canopy Kernel + Zerp -- "
                          "the quick brown fox jumps over the lazy dog 1234567890 "
                          "the quick brown fox jumps over the lazy dog 1234567890";

int main(void) {
    uLong srclen = (uLong)strlen(msg) + 1;
    uLong complen = compressBound(srclen);
    unsigned char *compbuf = malloc(complen);
    unsigned char *outbuf = malloc(srclen);

    int rc = compress(compbuf, &complen, (const unsigned char *)msg, srclen);
    printf("compress() rc=%d srclen=%lu complen=%lu\n", rc, (unsigned long)srclen, (unsigned long)complen);
    if (rc != Z_OK) { printf("FAIL: compress\n"); return 1; }

    uLong destlen = srclen;
    rc = uncompress(outbuf, &destlen, compbuf, complen);
    printf("uncompress() rc=%d destlen=%lu\n", rc, (unsigned long)destlen);
    if (rc != Z_OK) { printf("FAIL: uncompress\n"); return 1; }

    if (destlen == srclen && memcmp(outbuf, msg, srclen) == 0) {
        printf("PASS: round-trip matches byte-for-byte via real zlib on WynlandOS\n");
        printf("decoded: %s\n", outbuf);
        return 0;
    }
    printf("FAIL: round-trip mismatch\n");
    return 1;
}
