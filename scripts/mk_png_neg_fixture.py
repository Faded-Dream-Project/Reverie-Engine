#!/usr/bin/env python3
"""mk_png_neg_fixture.py — generates the malformed-PNG fixtures used by
tests/test_png.c's negative legs.

Everything except the decompression bomb is crafted in C by the test itself
(chunk builder + rev_crc32); the bomb needs real compressed data, so it is
generated here once and committed:

  png_bomb.png   declares a 2x1 grayscale image (expected raw size = 3 bytes:
                 2 filter bytes + 2 pixel bytes) whose IDAT is a valid zlib
                 stream that inflates to 200,000 zero bytes. Pre-hard-cap the
                 inflater grew the output buffer geometrically toward 256 KiB
                 before the size check ran — with a larger declared payload
                 the same shape is the classic decompression bomb. The fixture
                 pins the REV_INF_TOOBIG refusal.

Run from the repo root:  python3 scripts/mk_png_neg_fixture.py
"""
import struct
import sys
import zlib


def chunk(typ: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + typ + data +
            struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF))


def png(sig: bytes, ihdr: bytes, idat: bytes) -> bytes:
    return sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")


def main() -> int:
    out = "tests/fixtures/png_bomb.png"
    # IHDR: 2x1, bit depth 8, color type 0 (gray), no interlace
    ihdr = struct.pack(">IIBBBBB", 2, 1, 8, 0, 0, 0, 0)
    # 200,000 zero bytes as one zlib stream (~200 bytes stored? no — let
    # deflate actually compress it so the input stays tiny)
    payload = zlib.compress(b"\x00" * 200_000, 9)
    assert len(payload) < 400, len(payload)
    data = png(b"\x89PNG\r\n\x1a\n", ihdr, payload)
    with open(out, "wb") as f:
        f.write(data)
    print(f"wrote {out} ({len(data)} bytes, inflates to 200000, "
          f"declared budget 3)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
