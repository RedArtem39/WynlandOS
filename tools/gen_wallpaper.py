#!/usr/bin/env python3
"""WynlandOS default wallpaper: a dark gradient with soft colour glows.

Rendered at half resolution (the scene scales it up; it is all low
frequency anyway), written as a plain PNG with no third-party modules.
usage: gen_wallpaper.py out.png [width height]
"""
import math
import struct
import sys
import zlib

out = sys.argv[1]
W = int(sys.argv[2]) if len(sys.argv) > 2 else 960
H = int(sys.argv[3]) if len(sys.argv) > 3 else 540

# (cx, cy, radius, (r, g, b), strength) in 0..1 screen units
GLOWS = [
    (0.18, 0.28, 0.55, (122, 162, 247), 0.55),   # blue
    (0.82, 0.22, 0.50, (187, 154, 247), 0.50),   # violet
    (0.62, 0.86, 0.60, (51, 204, 255), 0.35),    # cyan
    (0.10, 0.95, 0.45, (160, 102, 255), 0.30),   # purple
]
TOP = (14, 16, 30)
BOTTOM = (8, 18, 30)


def pixel(x, y):
    u, v = x / W, y / H
    r = TOP[0] + (BOTTOM[0] - TOP[0]) * v
    g = TOP[1] + (BOTTOM[1] - TOP[1]) * v
    b = TOP[2] + (BOTTOM[2] - TOP[2]) * v
    for cx, cy, rad, col, s in GLOWS:
        dx = (u - cx) * (W / H)
        dy = v - cy
        k = s * math.exp(-(dx * dx + dy * dy) / (rad * rad * 0.35))
        r += (col[0] - r) * k
        g += (col[1] - g) * k
        b += (col[2] - b) * k
    # a little ordered dither against banding
    d = ((x * 7 + y * 13) % 4) - 1.5
    return (max(0, min(255, int(r + d))), max(0, min(255, int(g + d))), max(0, min(255, int(b + d))))


rows = []
for y in range(H):
    row = bytearray(b'\x00')
    for x in range(W):
        row.extend(pixel(x, y))
    rows.append(bytes(row))


def chunk(t, data):
    return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data) & 0xffffffff)


png = (b'\x89PNG\r\n\x1a\n'
       + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(b''.join(rows), 9))
       + chunk(b'IEND', b''))
open(out, 'wb').write(png)
print(f'  WALL       {out} ({W}x{H})')
