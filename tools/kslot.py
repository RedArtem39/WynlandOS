#!/usr/bin/env python3
"""
WynlandOS - a kernel for a live stick's kernel slot (boot/boot.c SlotKernel).

Copies the kernel and writes its slot header: "WYNKERN1", u32 bytes, u32
FNV-1a of them, 64 bytes of text (when and what), to 512 bytes. The machine
fetches both from tools/devhost.py and writes them with `test-wyn k`.

Usage: python3 tools/kslot.py build/kernel.elf build/devhost/kernel.elf build/devhost/kernel.hdr
"""
import shutil
import struct
import subprocess
import sys
import time


def main() -> None:
    src, dst, hdr = sys.argv[1:4]
    data = open(src, 'rb').read()
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    try:
        rev = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip()
    except OSError:
        rev = '?'
    what = f"{time.strftime('%Y-%m-%d %H:%M')} {rev}+, {len(data) // 1024} KB"[:64].encode()
    shutil.copyfile(src, dst)
    open(hdr, 'wb').write((b'WYNKERN1' + struct.pack('<II', len(data), h) + what.ljust(64, b'\0')).ljust(512, b'\0'))
    print(f'  kslot      {dst}: {what.decode()}')


if __name__ == '__main__':
    main()
