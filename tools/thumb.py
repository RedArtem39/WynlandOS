#!/usr/bin/env python3
"""WynlandOS - the frame thumbnails simpledrm logs (drivers/video/virtgpu_drm.c,
"simpledrm: thumb" lines, 48 x 27 pixels) as pictures: thumb-N.png (or .ppm
without Pillow), each pixel 16 x 16.

    tools/thumb.py build/netlog/boot-*.log [-o DIR]
"""
import re
import sys
import os

args = sys.argv[1:]
out = '.'
if '-o' in args:
    i = args.index('-o')
    out = args[i + 1]
    del args[i:i + 2]
os.makedirs(out, exist_ok=True)

thumbs, cur, name = [], None, None
for path in args:
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.search(r'simpledrm: thumb of present (0x[0-9A-Fa-f]+)', line)
        if m:
            name = int(m.group(1), 16)
            cur = []
            thumbs.append((name, cur))
            continue
        m = re.search(r'simpledrm: thumb ((?:[0-9a-f]{6} ?)+)', line)
        if m and cur is not None and len(cur) < 27:
            cur.append([int(v, 16) for v in m.group(1).split()])

S = 16
for n, rows in thumbs:
    if not rows:
        continue
    w, h = len(rows[0]), len(rows)
    px = bytearray()
    for r in rows:
        line = bytearray()
        for v in r + [0] * (w - len(r)):
            line += bytes(((v >> 16) & 255, (v >> 8) & 255, v & 255)) * S
        px += bytes(line) * S
    try:
        from PIL import Image
        p = os.path.join(out, f'thumb-{n}.png')
        Image.frombytes('RGB', (w * S, h * S), bytes(px)).save(p)
    except ImportError:
        p = os.path.join(out, f'thumb-{n}.ppm')
        with open(p, 'wb') as f:
            f.write(b'P6 %d %d 255\n' % (w * S, h * S) + bytes(px))
    print(p, f'{w}x{h}', 'distinct colours:', len({v for r in rows for v in r}))
