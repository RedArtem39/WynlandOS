/*
 * WynlandOS - LibreSSL verification: real crypto, not just "it links."
 * Two independent checks against real LibreSSL library code:
 *   1. SHA256("abc") against the well-known published test vector.
 *   2. AES-256-CBC encrypt then decrypt round-trip via EVP, confirming
 *      the decrypted plaintext matches the original byte-for-byte.
 * Build:
 *   x86_64-linux-musl-gcc -O2 -o test_libressl.elf test_libressl.c \
 *     -I<libressl>/build_musl/include -L<libressl>/build_musl/crypto \
 *     -lcrypto
 */
#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

static void hex(const unsigned char *buf, int len, char *out) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        out[i * 2]     = h[(buf[i] >> 4) & 0xF];
        out[i * 2 + 1] = h[buf[i] & 0xF];
    }
    out[len * 2] = 0;
}

int main(void) {
    /* --- SHA256 known-answer test --- */
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)"abc", 3, digest);
    char hexbuf[SHA256_DIGEST_LENGTH * 2 + 1];
    hex(digest, SHA256_DIGEST_LENGTH, hexbuf);
    printf("SHA256(\"abc\") = %s\n", hexbuf);

    const char *known = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    int sha_ok = (strcmp(hexbuf, known) == 0);
    printf("SHA256 known-answer check: %s\n", sha_ok ? "PASS" : "FAIL");

    /* --- AES-256-CBC round trip via EVP --- */
    unsigned char key[32];
    unsigned char iv[16];
    for (int i = 0; i < 32; i++) key[i] = (unsigned char)i;
    for (int i = 0; i < 16; i++) iv[i] = (unsigned char)(0x40 + i);

    const char *plaintext = "WynlandOS LibreSSL real AES-256-CBC round-trip test payload!!!";
    int plen = (int)strlen(plaintext);

    unsigned char ciphertext[256];
    unsigned char decrypted[256];
    int clen = 0, dlen = 0, tmplen = 0;

    EVP_CIPHER_CTX *ectx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ectx, EVP_aes_256_cbc(), NULL, key, iv);
    EVP_EncryptUpdate(ectx, ciphertext, &clen, (const unsigned char *)plaintext, plen);
    EVP_EncryptFinal_ex(ectx, ciphertext + clen, &tmplen);
    clen += tmplen;
    EVP_CIPHER_CTX_free(ectx);

    EVP_CIPHER_CTX *dctx = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(dctx, EVP_aes_256_cbc(), NULL, key, iv);
    EVP_DecryptUpdate(dctx, decrypted, &dlen, ciphertext, clen);
    EVP_DecryptFinal_ex(dctx, decrypted + dlen, &tmplen);
    dlen += tmplen;
    EVP_CIPHER_CTX_free(dctx);

    decrypted[dlen] = 0;
    printf("AES-256-CBC ciphertext length = %d, decrypted length = %d\n", clen, dlen);
    printf("AES-256-CBC decrypted text = %s\n", decrypted);

    int aes_ok = (dlen == plen) && (memcmp(decrypted, plaintext, plen) == 0);
    printf("AES-256-CBC round-trip check: %s\n", aes_ok ? "PASS" : "FAIL");

    if (sha_ok && aes_ok) {
        printf("PASS: real LibreSSL crypto (SHA256 + AES-256-CBC via EVP) works on WynlandOS\n");
        return 0;
    }
    printf("FAIL: one or more LibreSSL checks failed\n");
    return 1;
}
