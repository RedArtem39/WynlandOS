/*
 * WynlandOS - livepack: the live stick's system, packed (make live).
 *
 *   livepack <disk.img> <stick.img>
 *
 * The loader (boot/boot.c, LivePacked) reads the system into RAM through
 * the firmware's USB driver -- 15-25 MB/s on a real machine: the whole
 * 2 GB took minutes. Packed, it reads only what is there:
 *
 *   stick.img = disk.img's LBA 0 up to its root partition (the MBR and the
 *               ESP the firmware boots from), then the pack where the root
 *               partition was (partition 2, type 0xDA: no file system),
 *               then 4 MB for the running kernel's log (partition 3, 0x7F)
 *               and 16 MB for a kernel sent over the network (partition 4,
 *               0x7E: boot/boot.c SlotKernel)
 *   pack      = sector 0: "WYNPACK2", u64 bytes (disk.img from LBA 0 to the
 *               end of its root partition), u32 chunks, u32 chunk size;
 *               from sector 1: per chunk its packed size, u32 (0: all zero,
 *               the chunk size: stored as is, else an LZ4 block) and the
 *               sum of it unpacked, u32 (piece_sum); then the chunks, each
 *               from a sector boundary
 *
 * The compressor is a plain greedy LZ4 (block format): no library needed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK   (4u << 20)          /* RAMDISK_CHUNK (include/wynland/boot_info.h) */
#define HBITS   16
#define KLOG_SECTORS 8192u            /* 4 MB: the kernel log area, partition 3 */
#define SLOT_SECTORS 32768u           /* 16 MB: the kernel slot, partition 4 */

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* a piece's sum, unpacked (boot/boot.c PieceSum: the same): FNV-1a over
   its 64-bit words, folded -- the loader checks every piece it unpacks */
static uint32_t piece_sum(const uint8_t *p)
{
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; i < CHUNK; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ (uint32_t)w ^ (uint32_t)(w >> 32)) * 16777619u;
    }
    return h;
}

static uint8_t *put_len(uint8_t *op, size_t n)
{
    for (; n >= 255; n -= 255) *op++ = 255;
    *op++ = (uint8_t)n;
    return op;
}

static uint8_t *put_seq(uint8_t *op, const uint8_t *lit, size_t nlit, size_t off, size_t mlen)
{
    uint8_t *tok = op++;
    *tok = (uint8_t)((nlit >= 15 ? 15 : nlit) << 4);
    if (nlit >= 15) op = put_len(op, nlit - 15);
    memcpy(op, lit, nlit);
    op += nlit;
    if (!mlen) return op;                       /* the last literals */
    *op++ = (uint8_t)off;
    *op++ = (uint8_t)(off >> 8);
    mlen -= 4;
    *tok |= (uint8_t)(mlen >= 15 ? 15 : mlen);
    if (mlen >= 15) op = put_len(op, mlen - 15);
    return op;
}

/* LZ4's own rules: the last 5 bytes are literals, no match starts in the
   last 12 */
static size_t lz4_pack(const uint8_t *src, size_t n, uint8_t *dst)
{
    static uint32_t ht[1u << HBITS];
    memset(ht, 0, sizeof ht);
    const uint8_t *ip = src, *anchor = src, *end = src + n;
    const uint8_t *limit = n > 12 ? end - 12 : src;
    uint8_t *op = dst;
    while (ip < limit) {
        uint32_t seq = rd32(ip);
        uint32_t h = (seq * 2654435761u) >> (32 - HBITS);
        uint32_t ref = ht[h];
        ht[h] = (uint32_t)(ip - src) + 1;
        const uint8_t *m = src + ref - 1;
        if (!ref || ip - m > 65535 || rd32(m) != seq) { ip++; continue; }
        size_t len = 4;
        while (ip + len < end - 5 && ip[len] == m[len]) len++;
        op = put_seq(op, anchor, (size_t)(ip - anchor), (size_t)(ip - m), len);
        ip += len;
        anchor = ip;
    }
    return (size_t)(put_seq(op, anchor, (size_t)(end - anchor), 0, 0) - dst);
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: livepack <disk.img> <stick.img>\n"); return 1; }
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    if (!in || !out) { perror("livepack"); return 1; }

    uint8_t mbr[512];
    if (fread(mbr, 1, 512, in) != 512 || mbr[510] != 0x55 || mbr[511] != 0xAA || mbr[0x1CE + 4] != 0x83) {
        fprintf(stderr, "livepack: %s has no WynlandOS partition table\n", argv[1]);
        return 1;
    }
    uint32_t root = rd32(mbr + 0x1CE + 8), count = rd32(mbr + 0x1CE + 12);
    uint64_t bytes = ((uint64_t)root + count) * 512;
    uint32_t nchunks = (uint32_t)((bytes + CHUNK - 1) / CHUNK);
    uint32_t tsec = (nchunks * 8 + 511) / 512;

    uint8_t *buf = malloc(CHUNK), *packed = malloc(CHUNK + CHUNK / 255 + 64);
    uint32_t *sizes = calloc(nchunks, 8);         /* per piece: packed size, sum of it unpacked */
    if (!buf || !packed || !sizes) { fprintf(stderr, "livepack: out of memory\n"); return 1; }

    /* the boot part as it is; its MBR then says where the pack is */
    uint64_t at = (uint64_t)root * 512 + 512 + (uint64_t)tsec * 512;
    fseek(in, 0, SEEK_SET);
    for (uint64_t done = 0; done < (uint64_t)root * 512; ) {
        size_t n = (uint64_t)root * 512 - done < CHUNK ? (size_t)((uint64_t)root * 512 - done) : CHUNK;
        if (fread(buf, 1, n, in) != n) { fprintf(stderr, "livepack: short read\n"); return 1; }
        fwrite(buf, 1, n, out);
        done += n;
    }

    uint64_t total = 0;
    fseek(out, (long)at, SEEK_SET);
    for (uint32_t c = 0; c < nchunks; c++) {
        size_t n = bytes - (uint64_t)c * CHUNK < CHUNK ? (size_t)(bytes - (uint64_t)c * CHUNK) : CHUNK;
        memset(buf, 0, CHUNK);
        fseek(in, (long)((uint64_t)c * CHUNK), SEEK_SET);
        if (fread(buf, 1, n, in) != n) { fprintf(stderr, "livepack: short read\n"); return 1; }
        size_t z = 0;
        while (z < CHUNK && !buf[z]) z++;
        if (z == CHUNK) { sizes[2 * c] = 0; continue; }
        sizes[2 * c + 1] = piece_sum(buf);
        size_t p = lz4_pack(buf, CHUNK, packed);
        const uint8_t *src = packed;
        if (p >= CHUNK) { p = CHUNK; src = buf; }
        sizes[2 * c] = (uint32_t)p;
        fwrite(src, 1, p, out);
        size_t pad = (512 - p % 512) % 512;
        static const uint8_t zero[512];
        fwrite(zero, 1, pad, out);
        total += p + pad;
    }

    uint8_t head[512] = "WYNPACK2";
    memcpy(head + 8, &bytes, 8);
    memcpy(head + 16, &nchunks, 4);
    uint32_t cs = CHUNK;
    memcpy(head + 20, &cs, 4);
    fseek(out, (long)((uint64_t)root * 512), SEEK_SET);
    fwrite(head, 1, 512, out);
    uint8_t *table = calloc(tsec, 512);
    memcpy(table, sizes, (size_t)nchunks * 8);
    fwrite(table, 1, (size_t)tsec * 512, out);

    /* partition 2 is now the pack: type 0xDA (no file system), its size */
    uint32_t psec = (uint32_t)((512 + (uint64_t)tsec * 512 + total) / 512);
    mbr[0x1CE + 4] = 0xDA;
    memcpy(mbr + 0x1CE + 12, &psec, 4);
    /* partition 3: 4 MB where the running kernel keeps its log (type 0x7F,
       kernel/sticklog.c; the next boot's loader saves it to \logs) */
    uint32_t kstart = (root + psec + 2047) / 2048 * 2048, kcount = KLOG_SECTORS;
    memset(mbr + 0x1DE, 0, 16);
    mbr[0x1DE + 4] = 0x7F;
    memcpy(mbr + 0x1DE + 8, &kstart, 4);
    memcpy(mbr + 0x1DE + 12, &kcount, 4);
    /* partition 4: 16 MB for a kernel sent from the developer's machine
       (type 0x7E; boot/boot.c SlotKernel boots it; empty: the ESP's) */
    uint32_t sstart = kstart + kcount, scount = SLOT_SECTORS;
    memset(mbr + 0x1EE, 0, 16);
    mbr[0x1EE + 4] = 0x7E;
    memcpy(mbr + 0x1EE + 8, &sstart, 4);
    memcpy(mbr + 0x1EE + 12, &scount, 4);
    {
        static uint8_t zero[512];
        fseek(out, (long)((uint64_t)kstart * 512), SEEK_SET);
        for (uint32_t i = 0; i < kcount + scount; i++) fwrite(zero, 1, 512, out);
    }
    fseek(out, 0, SEEK_SET);
    fwrite(mbr, 1, 512, out);
    fclose(out);
    fclose(in);
    printf("  livepack   %llu MB of system -> %llu MB packed\n",
           (unsigned long long)(bytes >> 20), (unsigned long long)((total + 512 + tsec * 512) >> 20));
    return 0;
}
