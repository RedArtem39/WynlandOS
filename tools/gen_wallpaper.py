#!/usr/bin/env python3
"""WynlandOS default wallpaper: near black, a little film grain, a soft
vignette, and the WynlandOS spiral (rootfs/usr/share/wynland/logo.png)
dimmed in the lower right corner. The palette is the logo's blue on
black -- nothing else.

Plain Python (the build host has no imaging modules): the logo PNG is
decoded here, the result written as a PNG.
usage: gen_wallpaper.py out.png [width height]
"""
import os
import random
import struct
import sys
import zlib

out = sys.argv[1]
W = int(sys.argv[2]) if len(sys.argv) > 2 else 1920
H = int(sys.argv[3]) if len(sys.argv) > 3 else 1080
LOGO = os.path.join(os.path.dirname(__file__), "..", "rootfs", "usr", "share", "wynland", "logo.png")


def read_png(path):
    """(w, h, rows of (r, g, b)) of an 8-bit RGB or RGBA PNG"""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat = 8, b""
    w = h = ctype = 0
    while pos < len(data):
        n, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if tag == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and ctype in (2, 6)
        elif tag == b"IDAT":
            idat += body
        pos += 12 + n
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    prev = bytearray(stride)
    rows = []
    i = 0
    for _ in range(h):
        f = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 255
        rows.append([(line[k], line[k + 1], line[k + 2]) for k in range(0, stride, bpp)])
        prev = line
    return w, h, rows


lw, lh, logo = read_png(LOGO)
size = int(H * 0.42)                       # the spiral: 42% of the height
ox, oy = W - size - int(H * 0.06), H - size - int(H * 0.06)
STRENGTH = 0.22                            # dimmed: a mark, not a poster

rng = random.Random(1)
rows = bytearray()
for y in range(H):
    rows.append(0)
    vy = (y / H - 0.5) * 2
    for x in range(W):
        vx = (x / W - 0.5) * 2
        vign = 1.0 - 0.35 * min(1.0, (vx * vx * 0.6 + vy * vy))
        base = (7 + 6 * (1 - y / H)) * vign   # a faint lift towards the top
        grain = rng.uniform(-2.2, 2.2)
        r = base + grain
        g = base + 1.0 + grain
        b = base + 4.0 + grain
        lx, ly = x - ox, y - oy
        if 0 <= lx < size and 0 <= ly < size:
            pr, pg, pb = logo[ly * lh // size][lx * lw // size]
            # the logo sits on black: "screen" it onto the background
            r = 255 - (255 - r) * (1 - pr * STRENGTH / 255)
            g = 255 - (255 - g) * (1 - pg * STRENGTH / 255)
            b = 255 - (255 - b) * (1 - pb * STRENGTH / 255)
        rows += bytes((max(0, min(255, int(r))), max(0, min(255, int(g))), max(0, min(255, int(b)))))


def chunk(tag, data):
    c = struct.pack(">I", len(data)) + tag + data
    return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
png += chunk(b"IEND", b"")
open(out, "wb").write(png)
print(f"  WALL       {out} ({W}x{H})")
