#!/usr/bin/env python3
"""Ripple click-placement comparator (e2e regression tool).

Compares a clicked screenshot against an unclicked baseline and verifies the
disturbance lands where the click was fired. Used by scripts/e2e.sh to pin
the GPU ripple solver's click placement (the 2026-10 Laplacian/injection bug
made clicks land as a weak displaced smear).

Usage: click_cmp.py BASELINE.png CLICKED.png X Y [MIN_PCT]
   X, Y  click position in normalized [0..1] coordinates
Exit: 0 = pass, 1 = fail (reason on stdout).
"""
import struct
import sys
import zlib


def read_png(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG: ' + path)
    pos = 8
    w = h = None
    idat = b''
    while pos < len(data):
        ln = struct.unpack('>I', data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + ln]
        if typ == b'IHDR':
            w, h, bd, ct = struct.unpack('>IIBB', chunk[:10])
            if bd != 8 or ct != 6:
                raise ValueError('only 8-bit RGBA PNGs supported: ' + path)
        elif typ == b'IDAT':
            idat += chunk
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 4
    px = bytearray(w * h * 4)
    prev = bytearray(stride)
    rp = 0
    for y in range(h):
        ft = raw[rp]
        rp += 1
        line = bytearray(raw[rp:rp + stride])
        rp += stride
        if ft == 1:
            for i in range(4, stride):
                line[i] = (line[i] + line[i - 4]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - 4] if i >= 4 else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - 4] if i >= 4 else 0
                b = prev[i]
                c = prev[i - 4] if i >= 4 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        px[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, px


def main():
    if len(sys.argv) < 5:
        print('usage: click_cmp.py BASE CLICKED X Y [MIN_PCT]')
        return 1
    base, clicked = sys.argv[1], sys.argv[2]
    cx, cy = float(sys.argv[3]), float(sys.argv[4])
    min_pct = float(sys.argv[5]) if len(sys.argv) > 5 else 0.2
    w, h, bp = read_png(base)
    w2, h2, cp = read_png(clicked)
    if (w, h) != (w2, h2):
        print('FAIL: size mismatch %dx%d vs %dx%d' % (w, h, w2, h2))
        return 1
    n = 0
    sx = sy = 0.0
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 4
            d = abs(cp[i] - bp[i]) + abs(cp[i + 1] - bp[i + 1]) + abs(cp[i + 2] - bp[i + 2])
            if d > 12:
                n += 1
                sx += x
                sy += y
    pct = 100.0 * n / (w * h)
    if n == 0:
        print('FAIL: click produced no visible change')
        return 1
    gx, gy = sx / n, sy / n
    tol_x, tol_y = w * 0.06, h * 0.06
    ok = True
    if pct < min_pct:
        print('FAIL: only %.2f%% of pixels changed (need >= %.2f%%) '
              '— wave did not launch properly' % (pct, min_pct))
        ok = False
    if abs(gx - cx * w) > tol_x or abs(gy - cy * h) > tol_y:
        print('FAIL: disturbance centroid (%.0f,%.0f) not at click (%.0f,%.0f) '
              '± (%.0f,%.0f)' % (gx, gy, cx * w, cy * h, tol_x, tol_y))
        ok = False
    if ok:
        print('PASS: %.2f%% changed, centroid (%.0f,%.0f) on target' % (pct, gx, gy))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
