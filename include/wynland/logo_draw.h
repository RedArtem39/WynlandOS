/*
 * WynlandOS - the logo, drawn from its vectors (assets/logo/wynland-logo.json,
 * as tables in logo_data.h) -- by the loader while it reads the system, and
 * by the kernel. Integers only: neither may touch the FPU.
 *
 * logo_draw() paints one frame into a size x size buffer (0x00RRGGBB):
 *   t_ms   time since the start: the blades wind in from the centre
 *          outwards (LOGO_ASSEMBLE_MS), each turning in to its place
 *   glow   0..256: a soft ring of light running outwards over it (phase
 *          in ms), for "still working"; 0 for none
 * Each blade: its outline filled with 4 sub-rows a pixel row and exact
 * horizontal coverage, its colour a cubic in its own frame per pixel.
 */
#pragma once

#include <wynland/types.h>
#include <wynland/logo_data.h>

#define LOGO_ASSEMBLE_MS 1600
#define LOGO_MAX_SIZE    1024
#define LOGO_MAX_X       64          /* crossings on one sub-row */

static int32_t  logo_cov[LOGO_MAX_SIZE + 2];

static inline int32_t logo_sin_at(int32_t a) { return logo_sin[a & 1023]; }
static inline int32_t logo_cos_at(int32_t a) { return logo_sin[(a + 256) & 1023]; }

/* 0..65536: ease-out cubic of x / span */
static inline int32_t logo_ease(int64_t x, int64_t span)
{
    if (x <= 0) return 0;
    if (x >= span) return 65536;
    int64_t r = 65536 - x * 65536 / span;          /* 1 - x */
    return (int32_t)(65536 - ((r * r >> 16) * r >> 16));
}

/* a span [xa, xb) of one sub-row (x in 1/256 px): its share of each pixel */
static inline void logo_span(int32_t xa, int32_t xb, int size)
{
    if (xa < 0) xa = 0;
    if (xb > size * 256) xb = size * 256;
    if (xb <= xa) return;
    int ia = xa >> 8, ib = xb >> 8;
    if (ia == ib) { logo_cov[ia] += (xb - xa) >> 2; return; }
    logo_cov[ia] += (256 - (xa & 255)) >> 2;
    for (int i = ia + 1; i < ib; i++) logo_cov[i] += 64;
    if (ib < size) logo_cov[ib] += (xb & 255) >> 2;
}

static inline int32_t logo_poly(const int16_t *k, int64_t u, int64_t v)
{
    int64_t uu = u * u >> 14, uv = u * v >> 14, vv = v * v >> 14;
    int64_t c = (int64_t)k[0] * 16384 + k[1] * u + k[2] * v + k[3] * uu + k[4] * uv + k[5] * vv +
                ((k[6] * uu >> 14) * u) + ((k[7] * uu >> 14) * v) + ((k[8] * vv >> 14) * u) + ((k[9] * vv >> 14) * v);
    c >>= 18;                                      /* Q14 and x16 */
    return c < 0 ? 0 : c > 255 ? 255 : (int32_t)c;
}

static inline void logo_blend(uint32_t *p, int32_t r, int32_t g, int32_t b, int32_t a)
{
    uint32_t o = *p;
    int32_t or_ = (o >> 16) & 255, og = (o >> 8) & 255, ob = o & 255;
    or_ += (r - or_) * a >> 8; og += (g - og) * a >> 8; ob += (b - ob) * a >> 8;
    *p = ((uint32_t)or_ << 16) | ((uint32_t)og << 8) | (uint32_t)ob;
}

/* one blade at angle `ang` (1/1024 turn), scale sc and opacity p (Q16) */
static void logo_blade(uint32_t *buf, int size, const LogoBlade *bl, int32_t ang, int32_t sc, int32_t p)
{
    static int32_t vx[256], vy[256];
    int n = bl->count;
    if (n > 256) n = 256;
    int64_t c = logo_cos_at(ang), s = logo_sin_at(ang);
    /* output px per 1/16 source px, Q16, times the scale */
    int64_t f = (int64_t)size * 65536 / (2 * LOGO_RADIUS16) * sc >> 16;
    int32_t ctr = size * 128;                      /* the centre, 1/256 px */
    int32_t miny = 1 << 30, maxy = -(1 << 30);
    for (int i = 0; i < n; i++) {
        int64_t x = logo_pts[2 * (bl->first + i)], y = logo_pts[2 * (bl->first + i) + 1];
        int64_t rx = (x * c - y * s) >> 14, ry = (x * s + y * c) >> 14;
        vx[i] = ctr + (int32_t)(rx * f >> 8);
        vy[i] = ctr + (int32_t)(ry * f >> 8);
        if (vy[i] < miny) miny = vy[i];
        if (vy[i] > maxy) maxy = vy[i];
    }
    int y0 = miny >> 8, y1 = (maxy >> 8) + 1;
    if (y0 < 0) y0 = 0;
    if (y1 > size) y1 = size;

    /* the colour frame: u, v (Q24) as affine functions of the pixel */
    int64_t d = (int64_t)bl->size16 * f;            /* the frame's size in output px, Q16 */
    if (d <= 0) return;
    int64_t ux = c * ((int64_t)1 << 26) / d, uy = s * ((int64_t)1 << 26) / d;   /* Q24 per px */
    int64_t u0 = -(int64_t)bl->cx16 * ((int64_t)1 << 24) / bl->size16;
    int64_t v0 = -(int64_t)bl->cy16 * ((int64_t)1 << 24) / bl->size16;

    int32_t xs[LOGO_MAX_X]; int8_t ws[LOGO_MAX_X];
    for (int y = y0; y < y1; y++) {
        int xmin = size, xmax = -1;
        for (int sub = 0; sub < 4; sub++) {
            int32_t sy = y * 256 + 32 + sub * 64;
            int nx = 0;
            for (int i = 0, j = n - 1; i < n; j = i++) {
                int32_t ay = vy[j], by = vy[i];
                if ((ay <= sy) == (by <= sy)) continue;
                int32_t x = vx[j] + (int32_t)((int64_t)(sy - ay) * (vx[i] - vx[j]) / (by - ay));
                int8_t w = by > ay ? 1 : -1;
                if (nx < LOGO_MAX_X) {
                    int k = nx++;
                    while (k > 0 && xs[k - 1] > x) { xs[k] = xs[k - 1]; ws[k] = ws[k - 1]; k--; }
                    xs[k] = x; ws[k] = w;
                }
            }
            int wind = 0;
            for (int k = 0; k + 1 < nx; k++) {
                wind += ws[k];
                if (!wind) continue;
                int a = xs[k] >> 8, b = (xs[k + 1] >> 8) + 1;
                if (a < xmin) xmin = a;
                if (b > xmax) xmax = b;
                logo_span(xs[k], xs[k + 1], size);
            }
        }
        if (xmin < 0) xmin = 0;
        if (xmax > size) xmax = size;
        int32_t dy = y * 256 + 128 - ctr;
        for (int x = xmin; x < xmax; x++) {
            int32_t cv = logo_cov[x];
            if (!cv) continue;
            logo_cov[x] = 0;
            if (cv > 256) cv = 256;
            int32_t a = (int32_t)((int64_t)cv * p >> 16);
            int64_t dx = x * 256 + 128 - ctr;
            /* the pixel back in the blade's own (unturned, unscaled) frame */
            int64_t u = ((dx * ux + (int64_t)dy * uy) >> 8) + u0;
            int64_t v = ((-dx * uy + (int64_t)dy * ux) >> 8) + v0;
            u >>= 10; v >>= 10;                     /* Q14 */
            logo_blend(&buf[y * size + x], logo_poly(bl->k[0], u, v), logo_poly(bl->k[1], u, v),
                       logo_poly(bl->k[2], u, v), a);
        }
    }
}

/* the thin dark lines between blades: dots every half pixel, bilinear */
static void logo_draw_seams(uint32_t *buf, int size, int32_t p)
{
    int64_t f = (int64_t)size * 65536 / (2 * LOGO_RADIUS16);
    int32_t ctr = size * 128;
    int32_t a = (int32_t)(40LL * p >> 16);         /* two dots a px: ~0.3 a line */
    for (int s = 0; s < LOGO_NSEAMS; s++) {
        const LogoSeam *sm = &logo_seams[s];
        for (int i = 0; i + 1 < sm->count; i++) {
            int32_t ax = ctr + (int32_t)(logo_seam_pts[2 * (sm->first + i)] * f >> 8);
            int32_t ay = ctr + (int32_t)(logo_seam_pts[2 * (sm->first + i) + 1] * f >> 8);
            int32_t bx = ctr + (int32_t)(logo_seam_pts[2 * (sm->first + i + 1)] * f >> 8);
            int32_t by = ctr + (int32_t)(logo_seam_pts[2 * (sm->first + i + 1) + 1] * f >> 8);
            int32_t len = (bx > ax ? bx - ax : ax - bx) + (by > ay ? by - ay : ay - by);
            int steps = len / 128 + 1;
            for (int k = 0; k < steps; k++) {
                int32_t px = ax + (bx - ax) * k / steps - 128, py = ay + (by - ay) * k / steps - 128;
                int ix = px >> 8, iy = py >> 8, fx = px & 255, fy = py & 255;
                if (ix < 0 || iy < 0 || ix + 1 >= size || iy + 1 >= size) continue;
                int32_t w[4] = { (256 - fx) * (256 - fy) >> 8, fx * (256 - fy) >> 8, (256 - fx) * fy >> 8, fx * fy >> 8 };
                uint32_t *q[4] = { &buf[iy * size + ix], &buf[iy * size + ix + 1], &buf[(iy + 1) * size + ix], &buf[(iy + 1) * size + ix + 1] };
                for (int m = 0; m < 4; m++)
                    if (*q[m]) logo_blend(q[m], 8, 22, 110, a * w[m] >> 8);
            }
        }
    }
}

static void logo_draw(uint32_t *buf, int size, uint32_t t_ms, int32_t glow, uint32_t phase_ms)
{
    if (size > LOGO_MAX_SIZE) size = LOGO_MAX_SIZE;
    for (int i = 0; i < size * size; i++) buf[i] = 0;
    int32_t rmax = 1;
    for (int i = 0; i < LOGO_NBLADES; i++) if (logo_blades[i].r16 > rmax) rmax = logo_blades[i].r16;
    /* the outer ones first: the inner ones arrive first and stay on top */
    int32_t done = 65536;
    for (int pass = LOGO_NBLADES - 1; pass >= 0; pass--) {
        /* blades by distance, farthest first (a selection: 18 of them) */
        int pick = -1;
        for (int i = 0, rank; i < LOGO_NBLADES; i++) {
            rank = 0;
            for (int j = 0; j < LOGO_NBLADES; j++)
                if (logo_blades[j].r16 < logo_blades[i].r16 || (logo_blades[j].r16 == logo_blades[i].r16 && j < i)) rank++;
            if (rank == pass) { pick = i; break; }
        }
        const LogoBlade *bl = &logo_blades[pick];
        int64_t delay = (int64_t)LOGO_ASSEMBLE_MS * 45 / 100 * bl->r16 / rmax;
        int32_t p = logo_ease((int64_t)t_ms - delay, LOGO_ASSEMBLE_MS * 55 / 100);
        if (p < done) done = p;
        if (!p) continue;
        int32_t ang = -(int32_t)((int64_t)(65536 - p) * 427 >> 16);   /* 150 degrees = 427/1024 turn */
        int32_t sc = 49152 + (p >> 2);                                   /* 0.75 .. 1 */
        logo_blade(buf, size, bl, ang, sc, p);
    }
    /* the seams belong to the blades at rest: in over the last 15% */
    if (done > 55705) logo_draw_seams(buf, size, (int32_t)((int64_t)(done - 55705) * 65536 / 9831));

    if (glow > 0) {
        /* a ring of light, from the centre to the rim every 2.4 s */
        int32_t R = (int32_t)((uint64_t)(phase_ms % 2400) * (size / 2 + size / 8) / 2400);
        int32_t wdt = size / 10 + 1;
        int32_t h = size / 2;
        for (int y = 0; y < size; y++) {
            int32_t dy = y - h;
            for (int x = 0; x < size; x++) {
                uint32_t o = buf[y * size + x];
                if (!o) continue;
                int32_t dx = x - h;
                int32_t r2 = dx * dx + dy * dy;
                /* |r - R| ~ |r^2 - R^2| / 2R, near the ring */
                int32_t dist = R ? (r2 - R * R) / (2 * R) : wdt;
                if (dist < 0) dist = -dist;
                if (dist >= wdt) continue;
                int32_t k = (wdt - dist) * 256 / wdt;          /* 0..256, a tent */
                k = k * k >> 8;
                k = k * glow >> 8;
                k = k * 90 >> 8;                               /* at most ~0.35 */
                logo_blend(&buf[y * size + x], 255, 255, 255, k);
            }
        }
    }
}
