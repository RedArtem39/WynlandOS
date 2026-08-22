import struct, zlib, sys

def ppm_to_png(ppm_path, png_path):
    with open(ppm_path, 'rb') as f:
        data = f.read()
    assert data[:2] == b'P6'
    idx = 2
    vals = []
    while len(vals) < 3:
        while data[idx] in b' \t\r\n':
            idx += 1
        if data[idx:idx+1] == b'#':
            while data[idx] not in b'\r\n':
                idx += 1
            continue
        start = idx
        while data[idx] not in b' \t\r\n':
            idx += 1
        vals.append(int(data[start:idx]))
    idx += 1
    width, height, maxval = vals
    pixels = data[idx:idx + width * height * 3]

    raw = bytearray()
    for y in range(height):
        raw.append(0)
        row_start = y * width * 3
        raw += pixels[row_start:row_start + width * 3]

    def chunk(tag, payload):
        return struct.pack('>I', len(payload)) + tag + payload + struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff)

    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    idat = zlib.compress(bytes(raw), 6)

    with open(png_path, 'wb') as f:
        f.write(sig)
        f.write(chunk(b'IHDR', ihdr))
        f.write(chunk(b'IDAT', idat))
        f.write(chunk(b'IEND', b''))

if __name__ == '__main__':
    ppm_to_png(sys.argv[1], sys.argv[2])
