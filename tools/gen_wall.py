#!/usr/bin/env python3
"""Generates build/wall.png -- a dark blue-violet gradient wallpaper
1920x1080-ish (1024x768 to match the guest display), pure zlib/struct,
no external deps."""
import zlib, struct

W, H = 1024, 768
rows = bytearray()
for y in range(H):
    rows.append(0)  # filter: none
    for x in range(W):
        t = (x + y) / (W + H)
        r = int(18 + 40 * t)
        g = int(16 + 24 * t)
        b = int(42 + 90 * t)
        # subtle vignette glow center-left
        dx, dy = (x - W * 0.33) / W, (y - H * 0.4) / H
        d2 = dx * dx + dy * dy
        glow = max(0, 1.0 - d2 * 3.0)
        r = min(255, int(r + 60 * glow))
        g = min(255, int(g + 35 * glow))
        b = min(255, int(b + 70 * glow))
        rows += bytes((r, g, b, 255))

def chunk(tag, data):
    c = struct.pack('>I', len(data)) + tag + data
    return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

png = b'\x89PNG\r\n\x1a\n'
png += chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 6, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(bytes(rows), 9))
png += chunk(b'IEND', b'')

open('build/wall.png', 'wb').write(png)
print('wall.png written:', len(png), 'bytes')
