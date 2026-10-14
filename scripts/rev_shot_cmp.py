#!/usr/bin/env python3
"""rev_shot_cmp.py — pixel-wise comparison of two reverie --shot PNGs.

Usage: rev_shot_cmp.py a.png b.png [max_mean [max_p95]]
Prints mean/p95/max channel diffs and exits non-zero when the mean (or p95)
exceeds the thresholds. Used by scripts/e2e.sh to pin GPU-tier compositing
parity against the CPU tier (tolerances cover float-vs-8bit rounding).
"""
import sys
import zlib
import struct


def read_png(path):
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos = 8
    w = h = None
    idat = b""
    while pos + 8 <= len(d):
        ln = struct.unpack(">I", d[pos:pos + 4])[0]
        typ = d[pos + 4:pos + 8]
        if typ == b"IHDR" and ln >= 13:
            w, h, bd, ct = struct.unpack(">IIBB", d[pos + 8:pos + 18])
            if ct != 6:
                raise SystemExit(f"{path}: not RGBA (color type {ct})")
        if typ == b"IDAT":
            idat += d[pos + 8:pos + 8 + ln]
        pos += 12 + ln
    raw = zlib.decompress(idat)
    bpp = 4
    stride = w * bpp + 1
    out = bytearray(w * h * bpp)
    prev = bytearray(w * bpp)
    for y in range(h):
        row = raw[y * stride:(y + 1) * stride]
        if len(row) < stride:
            raise SystemExit(f"{path}: truncated row {y}")
        f = row[0]
        cur = bytearray(row[1:])
        if f == 1:
            for i in range(bpp, len(cur)):
                cur[i] = (cur[i] + cur[i - bpp]) & 255
        elif f == 2:
            for i in range(len(cur)):
                cur[i] = (cur[i] + prev[i]) & 255
        elif f == 3:
            for i in range(len(cur)):
                left = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + (left + prev[i]) // 2) & 255
        elif f == 4:
            for i in range(len(cur)):
                a = cur[i - bpp] if i >= bpp else 0
                b_ = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b_ - c
                pa, pb, pc = abs(p - a), abs(p - b_), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b_ if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 255
        out[y * w * bpp:(y + 1) * w * bpp] = cur
        prev = cur
    return w, h, out


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    a_path, b_path = sys.argv[1], sys.argv[2]
    max_mean = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0
    max_p95 = float(sys.argv[4]) if len(sys.argv) > 4 else 12.0
    w1, h1, a = read_png(a_path)
    w2, h2, b = read_png(b_path)
    if (w1, h1) != (w2, h2):
        print(f"FAIL: size mismatch {w1}x{h1} vs {w2}x{h2}")
        return 1
    diffs = []
    n = len(a)
    step = 4 * 7  # sample every 7th pixel (all 4 channels)
    s = 0
    cnt = 0
    mx = 0
    for i in range(0, n, step):
        for c in range(4):
            d = abs(a[i + c] - b[i + c])
            diffs.append(d)
            s += d
            cnt += 1
            if d > mx:
                mx = d
    diffs.sort()
    mean = s / cnt
    p95 = diffs[int(len(diffs) * 0.95)]
    print(f"cmp {a_path} vs {b_path}: mean={mean:.2f} p95={p95} max={mx} "
          f"(limits: mean<={max_mean} p95<={max_p95})")
    if mean > max_mean or p95 > max_p95:
        print("FAIL: pixel parity outside tolerance")
        return 1
    print("PASS: pixels match within tolerance")
    return 0


if __name__ == "__main__":
    sys.exit(main())
