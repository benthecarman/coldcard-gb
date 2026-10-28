#!/usr/bin/env python3
#
# Compare a region of a test-runner screenshot with a reference image.
#
#   compare_png.py SHOT.png REFERENCE.png X Y
#
# The reference is compared with the SHOT region of the same size at X,Y.
# Pixels are compared as Game Boy shades (0-3, by brightness), so the exact
# palette and RGB565 rounding don't matter. Standard library only.
#
import struct, sys, zlib


def read_png(path):
    # returns (width, height, rows of (r, g, b) tuples); 8-bit only
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path + ': not a PNG'
    pos, idat, palette = 8, b'', None
    while pos < len(data):
        ln, typ = struct.unpack('>I4s', data[pos:pos+8])
        body = data[pos+8:pos+8+ln]
        if typ == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif typ == b'PLTE':
            palette = [tuple(body[i:i+3]) for i in range(0, len(body), 3)]
        elif typ == b'IDAT':
            idat += body
        pos += 12 + ln

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    assert interlace == 0, path + ': interlaced PNG'
    assert depth == 8 or (depth in (1, 2, 4) and channels == 1), path + ': unsupported depth'
    raw = zlib.decompress(idat)
    stride = (w * channels * depth + 7) // 8
    bpp = max(1, channels * depth // 8)         # filter unit, in bytes
    rows, prev = [], bytearray(stride)
    for y in range(h):
        ft = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 0xff
            elif ft == 2:
                line[i] = (line[i] + b) & 0xff
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xff
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xff
        prev = line

        px = []
        if depth < 8:
            # 1, 2 or 4 bit samples, packed from the high bits down
            per = 8 // depth
            mask = (1 << depth) - 1
            for x in range(w):
                v = (line[x // per] >> ((per - 1 - x % per) * depth)) & mask
                if ctype == 3:
                    px.append(palette[v])
                else:
                    g = v * 255 // mask
                    px.append((g, g, g))
            rows.append(px)
            continue

        for x in range(w):
            v = line[x * channels:(x + 1) * channels]
            if ctype == 3:
                px.append(palette[v[0]])
            elif ctype in (0, 4):
                px.append((v[0], v[0], v[0]))
            else:
                px.append(tuple(v[:3]))
        rows.append(px)

    return w, h, rows


def shade(rgb):
    # 0 = lightest .. 3 = darkest
    lum = (rgb[0] * 3 + rgb[1] * 6 + rgb[2]) / 10
    return 3 - min(3, int(lum * 4 / 256))


def main():
    shot, ref, x0, y0 = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    sw, sh, srows = read_png(shot)
    rw, rh, rrows = read_png(ref)
    assert x0 + rw <= sw and y0 + rh <= sh, 'reference does not fit in shot'

    bad = []
    for y in range(rh):
        for x in range(rw):
            if shade(srows[y0 + y][x0 + x]) != shade(rrows[y][x]):
                bad.append((x, y))

    if bad:
        print('%s: %d of %d pixels differ from %s; first: %s'
              % (shot, len(bad), rw * rh, ref, bad[:10]))
        sys.exit(1)

    print('%s: matches %s (%dx%d)' % (shot, ref, rw, rh))


if __name__ == '__main__':
    main()
