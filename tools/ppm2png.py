#!/usr/bin/env python3
"""ppm2png.py -- turn a QEMU screendump into a PNG.

Pure python: zlib for the deflate and a handful of bytes of header. The
sandbox has no image tools and no PIL, and a screenshot you cannot look at
is not worth taking.
"""
import sys, struct, zlib


def convert(src, dst):
    data = open(src, "rb").read()
    # P6\n<w> <h>\n<max>\n
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P6", parts[0]
    w, h = map(int, parts[1].split())
    pix = parts[3]

    raw = bytearray()
    for y in range(h):
        raw.append(0)                       # filter: none
        row = pix[y * w * 3:(y + 1) * w * 3]
        raw += row

    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(dst, "wb").write(png)
    print("%s -> %s (%dx%d, %d bytes)" % (src, dst, w, h, len(png)))


if __name__ == "__main__":
    convert(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else sys.argv[1] + ".png")
