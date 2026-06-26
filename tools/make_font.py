#!/usr/bin/env python3
import os
import sys
import gzip
import struct

def parse_psf(filepath):
    if filepath.endswith('.gz'):
        with gzip.open(filepath, 'rb') as f:
            data = f.read()
    else:
        with open(filepath, 'rb') as f:
            data = f.read()

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
        print(f"Detected PSF1: {num_glyphs} glyphs, size {width}x{height}, mode {mode:02X}")

        glyphs = []
        for i in range(num_glyphs):
            offset = header_size + i * glyph_size
            glyph = data[offset:offset + glyph_size]
            rows = list(glyph[:height])
            while len(rows) < 16:
                rows.append(0)
            glyphs.append(rows)

        unicode_map = {}
        if mode & 0x02: # PSF1_HAS_UNICODE_TABLE
            offset = header_size + num_glyphs * glyph_size
            for i in range(num_glyphs):
                chars = []
                while offset < len(data) - 1:
                    val = struct.unpack('<H', data[offset:offset+2])[0]
                    offset += 2
                    if val == 0xFFFF:
                        break
                    if val == 0xFFFE: # Sequence indicator
                        # Skip until next separator
                        while offset < len(data) - 1:
                            v = struct.unpack('<H', data[offset:offset+2])[0]
                            offset += 2
                            if v == 0xFFFF:
                                break
                        break
                    chars.append(val)
                if chars:
                    unicode_map[i] = "".join(chr(c) for c in chars)

        return width, height, glyphs, unicode_map

    # PSF2 format
    elif data[0] == 0x72 and data[1] == 0xb5 and data[2] == 0x4a and data[3] == 0x86:
        version, header_size, flags, num_glyphs, glyph_size, height, width = struct.unpack('<IIIIIII', data[4:32])
        print(f"Detected PSF2: {num_glyphs} glyphs, size {width}x{height}, flags {flags:08X}")

        glyphs = []
        for i in range(num_glyphs):
            offset = header_size + i * glyph_size
            glyph = data[offset:offset + glyph_size]
            rows = list(glyph[:height])
            while len(rows) < 16:
                rows.append(0)
            glyphs.append(rows)

        unicode_map = {}
        if flags & 0x01: # PSF2_HAS_UNICODE_TABLE
            offset = header_size + num_glyphs * glyph_size
            for i in range(num_glyphs):
                chars = []
                while offset < len(data):
                    b = data[offset]
                    offset += 1
                    if b == 0xFF:
                        break
                    chars.append(b)
                
                # Decode UTF-8 string
                if chars:
                    try:
                        s = bytes(chars).decode('utf-8')
                        parts = s.split('\xfe')
                        unicode_map[i] = parts[0]
                    except Exception:
                        pass

        return width, height, glyphs, unicode_map

    else:
        raise ValueError("Invalid PSF magic bytes")

def main():
    font_path = "/usr/share/consolefonts/FullCyrSlav-Terminus16.psf.gz"
    if not os.path.exists(font_path):
        font_path = "/usr/share/consolefonts/CyrSlav-Terminus16.psf.gz"
        if not os.path.exists(font_path):
            print(f"Error: Terminus Cyrillic font not found at {font_path}")
            sys.exit(1)

    print(f"Parsing Cyrillic font: {font_path}")
    width, height, glyphs, unicode_map = parse_psf(font_path)

    # Map unicode characters to glyph index
    uni_to_glyph = {}
    for glyph_idx, uni_chars in unicode_map.items():
        for char in uni_chars:
            uni_to_glyph[ord(char)] = glyph_idx

    # Define CP866 to Unicode mapping table
    cp866_map = {}
    
    # 0x80 - 0x9F: Russian А-Я (U+0410 - U+042F)
    for i in range(0x80, 0xA0):
        cp866_map[i] = 0x0410 + (i - 0x80)

    # 0xA0 - 0xAF: Russian а-п (U+0430 - U+043F)
    for i in range(0xA0, 0xB0):
        cp866_map[i] = 0x0430 + (i - 0xA0)

    # 0xE0 - 0xEF: Russian р-я (U+0440 - U+044F)
    for i in range(0xE0, 0xF0):
        cp866_map[i] = 0x0440 + (i - 0xE0)

    # 0xF0: Ё (U+0401)
    cp866_map[0xF0] = 0x0401
    # 0xF1: ё (U+0451)
    cp866_map[0xF1] = 0x0451

    # Construct the output font_8x16 array of 256 elements
    out_glyphs = []
    for idx in range(256):
        if idx < 128:
            # Keep original ASCII glyphs
            out_glyphs.append(glyphs[idx] if idx < len(glyphs) else [0]*16)
        else:
            # Map using CP866 unicode code point
            target_uni = cp866_map.get(idx, None)
            if target_uni and target_uni in uni_to_glyph:
                glyph_idx = uni_to_glyph[target_uni]
                out_glyphs.append(glyphs[glyph_idx])
            else:
                # Fallback to original font glyph at this index
                out_glyphs.append(glyphs[idx] if idx < len(glyphs) else [0]*16)

    # Write C header
    out_path = "include/wynland/font.h"
    with open(out_path, "w") as f:
        f.write("/*\n * Automatically generated from console font by tools/make_font.py\n */\n\n")
        f.write("#pragma once\n\n")
        f.write("#include <wynland/types.h>\n\n")
        f.write("#define FONT_WIDTH  8\n")
        f.write("#define FONT_HEIGHT 16\n\n")
        f.write("/* 256 characters of 8x16 font, each character is 16 bytes (one byte per row) */\n")
        f.write("static const uint8_t font_8x16[256][16] = {\n")
        
        for idx in range(256):
            glyph = out_glyphs[idx]
            hex_str = ", ".join(f"0x{b:02X}" for b in glyph)
            comment = f"/* 0x{idx:02X}"
            if 32 <= idx <= 126:
                comment += f" '{chr(idx)}'"
            elif 0x80 <= idx <= 0x9F:
                comment += f" RU_CAP_{idx - 0x80}"
            elif 0xA0 <= idx <= 0xAF:
                comment += f" RU_LOW_{idx - 0xA0}"
            elif 0xE0 <= idx <= 0xEF:
                comment += f" RU_LOW_{idx - 0xE0 + 16}"
            elif idx == 0xF0:
                comment += " RU_CAP_YO"
            elif idx == 0xF1:
                comment += " RU_LOW_YO"
            comment += " */"
            f.write(f"    {{ {hex_str} }}, {comment}\n")
            
        f.write("};\n")

    print(f"Successfully generated CP866 font header at {out_path}")

if __name__ == "__main__":
    main()
