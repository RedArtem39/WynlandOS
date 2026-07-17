#!/usr/bin/env python3
import os
import sys
import gzip
import struct

def parse_psf(filepath):
    # Read and uncompress if gzipped
    if filepath.endswith('.gz'):
        with gzip.open(filepath, 'rb') as f:
            data = f.read()
    else:
        with open(filepath, 'rb') as f:
            data = f.read()

    # Check magic
    if len(data) < 4:
        raise ValueError("File too short")

    # PSF1 format
    if data[0] == 0x36 and data[1] == 0x04:
        mode, charsize = struct.unpack('<BB', data[2:4])
        header_size = 4
        num_glyphs = 512 if (mode & 0x01) else 256
        width = 8
        height = charsize
        glyph_size = charsize
        print(f"Detected PSF1: {num_glyphs} glyphs, size {width}x{height}")
    # PSF2 format
    elif data[0] == 0x72 and data[1] == 0xb5 and data[2] == 0x4a and data[3] == 0x86:
        version, header_size, flags, num_glyphs, glyph_size, height, width = struct.unpack('<IIIIIII', data[4:32])
        print(f"Detected PSF2: {num_glyphs} glyphs, size {width}x{height}")
    else:
        raise ValueError("Invalid PSF magic bytes")

    # Extract glyph data
    glyphs = []
    for i in range(num_glyphs):
        offset = header_size + i * glyph_size
        glyph = data[offset:offset + glyph_size]
        # Keep only the rows corresponding to height, pad to 16 bytes if smaller
        rows = list(glyph[:height])
        while len(rows) < 16:
            rows.append(0)
        glyphs.append(rows)

    return width, height, glyphs

def main():
    font_path = "/usr/share/consolefonts/Lat15-Terminus16.psf.gz"
    if not os.path.exists(font_path):
        # Fallback to VGA16
        font_path = "/usr/share/consolefonts/Lat15-VGA16.psf.gz"
        if not os.path.exists(font_path):
            print(f"Error: standard fonts not found in /usr/share/consolefonts/")
            sys.exit(1)

    print(f"Parsing font: {font_path}")
    try:
        width, height, glyphs = parse_psf(font_path)
    except Exception as e:
        print(f"Error parsing font: {e}")
        sys.exit(1)

    # Write C header
    out_dir = "include/wynland"
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "font.h")
    
    with open(out_path, "w") as f:
        f.write("/*\n * Automatically generated from console font by tools/psf2c.py\n */\n\n")
        f.write("#pragma once\n\n")
        f.write("#include <wynland/types.h>\n\n")
        f.write("#define FONT_WIDTH  8\n")
        f.write("#define FONT_HEIGHT 16\n\n")
        f.write("/* 256 characters of 8x16 font, each character is 16 bytes (one byte per row) */\n")
        f.write("static const uint8_t font_8x16[256][16] = {\n")
        
        # Write glyphs for all 256 ASCII characters
        for idx in range(min(256, len(glyphs))):
            glyph = glyphs[idx]
            hex_str = ", ".join(f"0x{b:02X}" for b in glyph)
            comment = f"/* 0x{idx:02X}"
            if 32 <= idx <= 126:
                comment += f" '{chr(idx)}'"
            comment += " */"
            f.write(f"    {{ {hex_str} }}, {comment}\n")
            
        f.write("};\n")

    print(f"Font header written successfully to {out_path}")

if __name__ == "__main__":
    main()
