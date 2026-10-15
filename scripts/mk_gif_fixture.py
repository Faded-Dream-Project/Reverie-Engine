#!/usr/bin/env python3
"""Generates tests/fixtures/gif_anim_transparent.gif — a 2-frame 80x60
animated GIF with a 4-color global color table and a TRANSPARENT index:

  frame 0: every pixel = index 0 (fully transparent),  delay 50 (0.5 s)
  frame 1: every pixel = index 1 (opaque red),          delay 50 (0.5 s)

Written as uncompressed LZW (a CLEAR code before every literal, the
classic trick) so no encoder is needed and the bytes are fully
reproducible. FFmpeg's gif decoder reads it like any normal GIF; the
engine's transparency test asserts frame 0 decodes to alpha 0 and
frame 1 to opaque red.

Run from the repository root:
    python3 scripts/mk_gif_fixture.py
"""

import struct

W, H = 80, 60
DELAY = 50                     # hundredths of a second per frame
GCT = bytes([0, 0, 0,          # index 0: transparent (via the GCE index)
             255, 0, 0,        # index 1: red
             0, 255, 0,       # index 2: green
             0, 0, 255])      # index 3: blue


class BitWriter:
    """LSB-first bit packing into <=255-byte sub-blocks (GIF LZW data)."""

    def __init__(self):
        self.out = bytearray()
        self.block = bytearray()
        self.cur = 0
        self.nbits = 0

    def put(self, code, width):
        self.cur |= (code & ((1 << width) - 1)) << self.nbits
        self.nbits += width
        while self.nbits >= 8:
            self.block.append(self.cur & 0xFF)
            self.cur >>= 8
            self.nbits -= 8
            if len(self.block) == 255:
                self.out.append(255)
                self.out += self.block
                self.block = bytearray()

    def finish(self):
        if self.nbits:
            self.block.append(self.cur & 0xFF)
            self.cur = 0
            self.nbits = 0
        # end-of-information (257) at the 9-bit initial width
        self.put(257, 9)
        if self.block:
            self.out.append(len(self.block))
            self.out += self.block
            self.block = bytearray()
        self.out.append(0)      # block terminator
        return bytes(self.out)


def lzw_frame(indices):
    """Uncompressed LZW: CLEAR(256) before every literal keeps the code
    width at 9 bits forever — the decoder's dictionary never grows."""
    bw = BitWriter()
    for px in indices:
        bw.put(256, 9)          # clear
        bw.put(px, 9)           # literal
    return bw.finish()


def gce(delay, transparent_index):
    return (bytes([0x21, 0xF9, 0x04, 0x01]) +
            struct.pack("<H", delay) +
            bytes([transparent_index, 0x00]))


def image_descriptor(w, h):
    return bytes([0x2C]) + struct.pack("<HHHH", 0, 0, w, h) + bytes([0x00])


def frame(delay, index):
    data = lzw_frame([index] * (w_h := W * H)) if False else \
        lzw_frame([index] * (W * H))
    return (gce(delay, 0) +
            image_descriptor(W, H) +
            bytes([8]) +          # LZW minimum code size
            data)


gif = bytearray()
gif += b"GIF89a"
gif += struct.pack("<HH", W, H)
gif += bytes([0x81])              # GCT present, 2^(1+1) = 4 entries
gif += bytes([0x00])              # background color index
gif += bytes([0x00])              # pixel aspect ratio
gif += GCT
gif += frame(DELAY, 0)            # transparent frame
gif += frame(DELAY, 1)            # opaque red frame
gif += b";"                       # trailer

with open("tests/fixtures/gif_anim_transparent.gif", "wb") as f:
    f.write(gif)
print("wrote tests/fixtures/gif_anim_transparent.gif (%d bytes)" % len(gif))
