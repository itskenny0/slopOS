#!/usr/bin/env python3
"""PicoOS packer -- LZSS with a 4 KB window.

Why not just ship the programs as they are? Because they compress by about
half, and half of a 46 KB kernel is worth having on a machine with 1 MB of
memory. The decompressor on the other side is forty lines of C and needs no
memory beyond the output buffer, which matters more than the ratio: anything
with a probability table or a Huffman tree would cost more RAM at run time
than it saves on disk.

The stream is bytes only, no bit packing beyond the flag byte:

    flag byte, 8 flags, least significant bit first
      1 -> the next byte is a literal
      0 -> the next two bytes are a back-reference:
             byte0 = offset low 8 bits
             byte1 = (offset high 4 bits << 4) | (length - 3)
           offset is 1..4096 counted back from the current output position,
           length is 3..18

A match costs two bytes plus an eighth of a flag byte, a literal one plus an
eighth, so matches shorter than three bytes are never worth emitting -- hence
the minimum. The window is 4 KB because twelve bits of offset and four of
length pack into exactly two bytes, and because a .pico program is rarely
bigger than that anyway.
"""
import sys

WINDOW = 4096
MINLEN = 3
MAXLEN = 18


def compress(src: bytes) -> bytes:
    out = bytearray()
    pos = 0
    n = len(src)

    # hash chain: three-byte key -> the positions it was last seen at.
    # Without this the search is O(n * window) and packing a 20 KB program
    # takes long enough to be annoying.
    heads = {}
    chain = [-1] * n

    flags = 0
    nflags = 0
    chunk = bytearray()

    def flush():
        nonlocal flags, nflags, chunk
        if nflags:
            out.append(flags)
            out.extend(chunk)
            flags = 0
            nflags = 0
            chunk = bytearray()

    while pos < n:
        best_len = 0
        best_off = 0

        if pos + MINLEN <= n:
            key = src[pos:pos + MINLEN]
            cand = heads.get(bytes(key), -1)
            tries = 0
            # A distance of exactly 4096 does not fit: the encoding carries
            # twelve bits, so 4096 wraps to zero and the decoder reads it as
            # an impossible back-reference. The window is therefore 1..4095.
            # This sat here unnoticed until a program happened to repeat
            # itself at exactly that distance, which is the way this class of
            # bug always shows up: never in testing, once in real data.
            while cand >= 0 and pos - cand < WINDOW and tries < 64:
                tries += 1
                length = 0
                limit = min(MAXLEN, n - pos)
                while length < limit and src[cand + length] == src[pos + length]:
                    length += 1
                if length > best_len:
                    best_len = length
                    best_off = pos - cand
                    if length == MAXLEN:
                        break
                cand = chain[cand]

        if best_len >= MINLEN:
            emit = best_len
            chunk.append(best_off & 0xFF)
            chunk.append(((best_off >> 8) & 0x0F) << 4 | (emit - MINLEN))
        else:
            emit = 1
            flags |= 1 << nflags
            chunk.append(src[pos])

        nflags += 1
        if nflags == 8:
            flush()

        # index every position we pass over, matched or not
        for i in range(pos, min(pos + emit, n - MINLEN + 1)):
            key = bytes(src[i:i + MINLEN])
            chain[i] = heads.get(key, -1)
            heads[key] = i
        pos += emit

    flush()
    return bytes(out)


def decompress(src: bytes, orig_size: int) -> bytes:
    """Reference decoder -- the C one in kernel/unpack.c must agree with it."""
    out = bytearray()
    i = 0
    while len(out) < orig_size:
        if i >= len(src):
            raise ValueError("stream ran out")
        flags = src[i]
        i += 1
        for bit in range(8):
            if len(out) >= orig_size:
                break
            if flags & (1 << bit):
                out.append(src[i])
                i += 1
            else:
                b0 = src[i]
                b1 = src[i + 1]
                i += 2
                off = b0 | ((b1 >> 4) << 8)
                ln = (b1 & 0x0F) + MINLEN
                if off == 0 or off > len(out):
                    raise ValueError("bad back-reference")
                start = len(out) - off
                for k in range(ln):
                    out.append(out[start + k])
    return bytes(out[:orig_size])


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: lzss.py <in> <out>   |   lzss.py -d <in> <out> <size>")
    if sys.argv[1] == "-d":
        data = open(sys.argv[2], "rb").read()
        open(sys.argv[3], "wb").write(decompress(data, int(sys.argv[4])))
        return
    raw = open(sys.argv[1], "rb").read()
    packed = compress(raw)
    # never make a file bigger by "compressing" it
    if len(packed) >= len(raw):
        print("  incompressible, stored as is")
    assert decompress(packed, len(raw)) == raw, "round trip failed"
    open(sys.argv[2], "wb").write(packed)
    pct = 100 * len(packed) // max(1, len(raw))
    print(f"  {len(raw)} -> {len(packed)} bytes ({pct}%)")


if __name__ == "__main__":
    main()
