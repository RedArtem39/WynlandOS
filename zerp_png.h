/*
 * WynlandOS / Zerp - minimal from-scratch PNG decoder.
 * No external dependency (matches this project's freestanding-binary
 * convention -- same spirit as ppm_to_png.py's from-scratch PNG
 * *encoder*, just the decode direction and in C). Implements just enough
 * of RFC 1950 (zlib wrapper) and RFC 1951 (DEFLATE: stored, fixed
 * Huffman, and dynamic Huffman blocks) to decode real-world PNGs,
 * plus PNG's own chunk framing and all 5 per-scanline filter types
 * (None/Sub/Up/Average/Paeth).
 *
 * Scope, deliberately bounded for v0: 8-bit depth, color type 2 (RGB) or
 * 6 (RGBA), non-interlaced only -- covers anything ppm_to_png.py (this
 * session's screendump-to-PNG tool) produces, and the overwhelming
 * majority of ordinary PNGs. Palette/grayscale/16-bit/interlaced PNGs
 * are rejected cleanly (ok=0), not silently misdecoded.
 *
 * No malloc anywhere in this OS's freestanding binaries -- scratch space
 * for the compressed IDAT bytes and the raw (filtered) scanline buffer
 * are static BSS arrays here, sized generously for screendump-scale
 * images. The final RGBA output goes into a buffer the CALLER supplies
 * (e.g. straight into their own SHM display buffer).
 */
#ifndef ZERP_PNG_H
#define ZERP_PNG_H

#include "zerp_syscalls.h"

#define ZERP_PNG_MAX_COMPRESSED (1u * 1024 * 1024)   /* 1 MB compressed IDAT scratch */
#define ZERP_PNG_MAX_RAW        (4u * 1024 * 1024)   /* 4 MB unfiltered scanline scratch */

typedef struct {
    uint32_t width, height;
    int ok;
} ZerpPngInfo;

/* ---------------- bit reader (LSB-first within each byte, per DEFLATE) --------------- */
typedef struct {
    const uint8_t *data;
    uint32_t len;
    uint32_t byte_pos;
    uint32_t bit_pos;
} ZPBitReader;

static uint32_t zp_read_bit(ZPBitReader *br) {
    if (br->byte_pos >= br->len) return 0;
    uint32_t bit = (br->data[br->byte_pos] >> br->bit_pos) & 1u;
    br->bit_pos++;
    if (br->bit_pos == 8) { br->bit_pos = 0; br->byte_pos++; }
    return bit;
}
static uint32_t zp_read_bits(ZPBitReader *br, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= zp_read_bit(br) << i;
    return v;
}
static void zp_align_byte(ZPBitReader *br) {
    if (br->bit_pos != 0) { br->bit_pos = 0; br->byte_pos++; }
}

/* ---------------- canonical Huffman decode (RFC 1951 3.2.2) --------------- */
typedef struct {
    uint16_t counts[16];
    uint16_t symbols_sorted[288];
    uint16_t first_code[16];
    uint16_t first_symbol_idx[16];
} ZPHuff;

static void zp_huff_build(ZPHuff *h, const uint8_t *lengths, int n) {
    for (int i = 0; i < 16; i++) h->counts[i] = 0;
    for (int i = 0; i < n; i++) h->counts[lengths[i]]++;
    h->counts[0] = 0;

    uint16_t offs[16];
    offs[0] = 0;
    for (int len = 1; len < 16; len++) offs[len] = (uint16_t)(offs[len - 1] + h->counts[len - 1]);
    for (int i = 0; i < n; i++) {
        if (lengths[i]) h->symbols_sorted[offs[lengths[i]]++] = (uint16_t)i;
    }

    uint16_t idx = 0, code = 0;
    for (int len = 1; len < 16; len++) {
        h->first_symbol_idx[len] = idx;
        h->first_code[len] = code;
        idx = (uint16_t)(idx + h->counts[len]);
        code = (uint16_t)((code + h->counts[len]) << 1);
    }
}

static int zp_huff_decode(ZPHuff *h, ZPBitReader *br) {
    int code = 0;
    for (int len = 1; len < 16; len++) {
        code = (code << 1) | (int)zp_read_bit(br);
        int cnt = h->counts[len];
        if (cnt && code >= h->first_code[len] && (code - h->first_code[len]) < cnt) {
            return h->symbols_sorted[h->first_symbol_idx[len] + (code - h->first_code[len])];
        }
    }
    return -1;
}

/* ---------------- DEFLATE (RFC 1951) --------------- */
static const uint16_t ZP_LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t ZP_LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t ZP_DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t ZP_DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};
static const uint8_t ZP_CLEN_ORDER[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

static void zp_inflate_block_huff(ZPBitReader *br, ZPHuff *lit, ZPHuff *dist,
                                   uint8_t *out, uint32_t *out_pos, uint32_t out_max) {
    for (;;) {
        int sym = zp_huff_decode(lit, br);
        if (sym < 0) return;
        if (sym < 256) {
            if (*out_pos < out_max) out[(*out_pos)++] = (uint8_t)sym;
        } else if (sym == 256) {
            return; /* end of block */
        } else {
            int li = sym - 257;
            if (li < 0 || li >= 29) return;
            uint32_t length = ZP_LEN_BASE[li] + zp_read_bits(br, ZP_LEN_EXTRA[li]);
            int dsym = zp_huff_decode(dist, br);
            if (dsym < 0 || dsym >= 30) return;
            uint32_t distance = ZP_DIST_BASE[dsym] + zp_read_bits(br, ZP_DIST_EXTRA[dsym]);
            if (distance > *out_pos) return; /* corrupt stream */
            uint32_t src = *out_pos - distance;
            for (uint32_t k = 0; k < length && *out_pos < out_max; k++) {
                out[*out_pos] = out[src + k];
                (*out_pos)++;
            }
        }
    }
}

static uint32_t zp_inflate(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_max) {
    ZPBitReader br; br.data = src; br.len = src_len; br.byte_pos = 0; br.bit_pos = 0;
    uint32_t out_pos = 0;

    static uint8_t fixed_lit_len[288];
    static uint8_t fixed_dist_len[30];
    static int fixed_built = 0;
    if (!fixed_built) {
        int i = 0;
        for (; i < 144; i++) fixed_lit_len[i] = 8;
        for (; i < 256; i++) fixed_lit_len[i] = 9;
        for (; i < 280; i++) fixed_lit_len[i] = 7;
        for (; i < 288; i++) fixed_lit_len[i] = 8;
        for (i = 0; i < 30; i++) fixed_dist_len[i] = 5;
        fixed_built = 1;
    }

    for (;;) {
        uint32_t bfinal = zp_read_bit(&br);
        uint32_t btype = zp_read_bits(&br, 2);

        if (btype == 0) {
            zp_align_byte(&br);
            if (br.byte_pos + 4 > br.len) break;
            uint32_t len = (uint32_t)br.data[br.byte_pos] | ((uint32_t)br.data[br.byte_pos + 1] << 8);
            br.byte_pos += 4; /* LEN + NLEN (NLEN unchecked) */
            for (uint32_t i = 0; i < len && br.byte_pos < br.len && out_pos < dst_max; i++) {
                dst[out_pos++] = br.data[br.byte_pos++];
            }
        } else if (btype == 1 || btype == 2) {
            static ZPHuff lit_tree, dist_tree;
            if (btype == 1) {
                zp_huff_build(&lit_tree, fixed_lit_len, 288);
                zp_huff_build(&dist_tree, fixed_dist_len, 30);
            } else {
                uint32_t hlit = zp_read_bits(&br, 5) + 257;
                uint32_t hdist = zp_read_bits(&br, 5) + 1;
                uint32_t hclen = zp_read_bits(&br, 4) + 4;

                uint8_t clen_lengths[19];
                for (int i = 0; i < 19; i++) clen_lengths[i] = 0;
                for (uint32_t i = 0; i < hclen; i++) {
                    clen_lengths[ZP_CLEN_ORDER[i]] = (uint8_t)zp_read_bits(&br, 3);
                }
                ZPHuff clen_tree;
                zp_huff_build(&clen_tree, clen_lengths, 19);

                static uint8_t all_lengths[288 + 32];
                uint32_t total = hlit + hdist;
                uint32_t i = 0;
                uint8_t prev = 0;
                while (i < total) {
                    int sym = zp_huff_decode(&clen_tree, &br);
                    if (sym < 0) return out_pos;
                    if (sym < 16) {
                        all_lengths[i++] = (uint8_t)sym;
                        prev = (uint8_t)sym;
                    } else if (sym == 16) {
                        uint32_t rep = zp_read_bits(&br, 2) + 3;
                        for (uint32_t r = 0; r < rep && i < total; r++) all_lengths[i++] = prev;
                    } else if (sym == 17) {
                        uint32_t rep = zp_read_bits(&br, 3) + 3;
                        for (uint32_t r = 0; r < rep && i < total; r++) all_lengths[i++] = 0;
                        prev = 0;
                    } else { /* 18 */
                        uint32_t rep = zp_read_bits(&br, 7) + 11;
                        for (uint32_t r = 0; r < rep && i < total; r++) all_lengths[i++] = 0;
                        prev = 0;
                    }
                }
                zp_huff_build(&lit_tree, all_lengths, (int)hlit);
                zp_huff_build(&dist_tree, all_lengths + hlit, (int)hdist);
            }
            zp_inflate_block_huff(&br, &lit_tree, &dist_tree, dst, &out_pos, dst_max);
        } else {
            break; /* reserved/error */
        }

        if (bfinal) break;
    }
    return out_pos;
}

/* ---------------- PNG filter reconstruction (RFC 2083 6.2/6.3) --------------- */
static uint8_t zp_paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    if (pb <= pc) return (uint8_t)b;
    return (uint8_t)c;
}

static void zp_unfilter(uint8_t *raw, uint32_t width, uint32_t height, int bpp) {
    uint32_t stride = width * (uint32_t)bpp;
    uint8_t *prev_row = (uint8_t *)0;
    for (uint32_t y = 0; y < height; y++) {
        uint8_t *row = raw + y * (stride + 1);
        uint8_t filter = row[0];
        uint8_t *px = row + 1;
        for (uint32_t x = 0; x < stride; x++) {
            int a = (x >= (uint32_t)bpp) ? px[x - bpp] : 0;
            int b = prev_row ? prev_row[x] : 0;
            int c = (prev_row && x >= (uint32_t)bpp) ? prev_row[x - bpp] : 0;
            switch (filter) {
                case 0: break;
                case 1: px[x] = (uint8_t)(px[x] + a); break;
                case 2: px[x] = (uint8_t)(px[x] + b); break;
                case 3: px[x] = (uint8_t)(px[x] + ((a + b) / 2)); break;
                case 4: px[x] = (uint8_t)(px[x] + zp_paeth(a, b, c)); break;
                default: break;
            }
        }
        prev_row = px;
    }
}

/* ---------------- PNG chunk parsing + top-level decode --------------- */
static uint32_t zp_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static ZerpPngInfo zerp_png_decode(const uint8_t *data, uint32_t len, uint32_t *out_rgba, uint32_t out_rgba_max_pixels) {
    static uint8_t compressed[ZERP_PNG_MAX_COMPRESSED];
    static uint8_t raw[ZERP_PNG_MAX_RAW];

    ZerpPngInfo info; info.width = 0; info.height = 0; info.ok = 0;

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (len < 8) return info;
    for (int i = 0; i < 8; i++) if (data[i] != sig[i]) return info;

    uint32_t pos = 8;
    uint32_t width = 0, height = 0;
    uint8_t bit_depth = 0, color_type = 0, interlace = 0;
    uint32_t comp_len = 0;
    int have_ihdr = 0;

    while (pos + 8 <= len) {
        uint32_t clen = zp_be32(data + pos);
        const uint8_t *ctype = data + pos + 4;
        const uint8_t *cdata = data + pos + 8;
        if (pos + 8 + clen + 4 > len) break;

        if (ctype[0] == 'I' && ctype[1] == 'H' && ctype[2] == 'D' && ctype[3] == 'R') {
            if (clen >= 13) {
                width = zp_be32(cdata);
                height = zp_be32(cdata + 4);
                bit_depth = cdata[8];
                color_type = cdata[9];
                interlace = cdata[12];
                have_ihdr = 1;
            }
        } else if (ctype[0] == 'I' && ctype[1] == 'D' && ctype[2] == 'A' && ctype[3] == 'T') {
            if (comp_len + clen <= ZERP_PNG_MAX_COMPRESSED) {
                for (uint32_t i = 0; i < clen; i++) compressed[comp_len + i] = cdata[i];
                comp_len += clen;
            }
        } else if (ctype[0] == 'I' && ctype[1] == 'E' && ctype[2] == 'N' && ctype[3] == 'D') {
            break;
        }

        pos += 8 + clen + 4;
    }

    if (!have_ihdr || bit_depth != 8 || interlace != 0) return info;
    if (color_type != 2 && color_type != 6) return info; /* RGB or RGBA only, v0 scope */
    if (width == 0 || height == 0 || width * height > out_rgba_max_pixels) return info;
    if (comp_len < 2) return info;

    int bpp = (color_type == 6) ? 4 : 3;
    uint32_t raw_needed = height * (width * (uint32_t)bpp + 1);
    if (raw_needed > ZERP_PNG_MAX_RAW) return info;

    /* Skip the 2-byte zlib header (RFC 1950); ignore the trailing 4-byte
       Adler-32 (not verified -- acceptable for this scope, matches the
       "documented limitation, not overclaimed" approach used for the
       SYS_wynland_elevate password gate elsewhere in this phase). */
    uint32_t inflated = zp_inflate(compressed + 2, comp_len - 2, raw, raw_needed);
    if (inflated < raw_needed) return info;

    zp_unfilter(raw, width, height, bpp);

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *row = raw + y * (width * (uint32_t)bpp + 1) + 1;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t r = row[x * (uint32_t)bpp + 0];
            uint8_t g = row[x * (uint32_t)bpp + 1];
            uint8_t b = row[x * (uint32_t)bpp + 2];
            uint8_t a = (bpp == 4) ? row[x * (uint32_t)bpp + 3] : 0xFF;
            out_rgba[y * width + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    info.width = width;
    info.height = height;
    info.ok = 1;
    return info;
}

#endif /* ZERP_PNG_H */
