#!/usr/bin/env python3
"""Turn a linked program into a .pico executable.

The image is the flat bytes objcopy produced. On top of that this adds two
things the kernel needs:

  * a 40-byte header -- magic, architecture, entry point, sizes, checksum
  * a relocation list

The relocation list is what lets a program run anywhere in memory. The
program is linked as if it will sit at 0x20000, which leaves absolute
addresses all over its code and data. `ld -q` keeps the relocation records
that say where those addresses are; this script boils them down to a plain
list of byte offsets, and the kernel adds "where it actually landed minus
0x20000" to each of them at load time.

That is the whole of PicoOS's dynamic loading. No symbols, no GOT, no PLT.
"""
import struct
import subprocess
import sys

MAGIC = 0x4F434950          # 'PICO'
FMT = 3
LINK_ADDR = 0x00020000
MAX_IMAGE = 0x00100000
API_MIN = 1
ARCH_X86_32 = 1
HDR_SIZE = 40


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def symbols(elf, nm):
    syms = {}
    for line in run([nm, elf]).splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def relocations(elf, readelf, image_size):
    """Byte offsets inside the image that hold an absolute address."""
    offsets = []
    unsupported = set()
    for line in run([readelf, "-r", elf]).splitlines():
        parts = line.split()
        if len(parts) < 3 or not parts[0].strip():
            continue
        try:
            where = int(parts[0], 16)
        except ValueError:
            continue
        kind = parts[2]
        if kind == "R_386_32":
            off = where - LINK_ADDR
            if 0 <= off < image_size:
                offsets.append(off)
        elif kind.startswith("R_386_"):
            # PC-relative ones need no fixing; anything else would mean the
            # program is not the simple flat binary we assume it is.
            if kind not in ("R_386_PC32", "R_386_NONE", "R_386_PLT32"):
                unsupported.add(kind)
    if unsupported:
        sys.exit(f"{elf}: cannot handle relocation types {sorted(unsupported)}. "
                 "Build the program with -fno-pie.")
    return sorted(set(offsets))


def main():
    if len(sys.argv) < 4:
        sys.exit("usage: mkpico.py <elf> <bin> <out.pico> [nm] [readelf]")
    elf, binary, out = sys.argv[1], sys.argv[2], sys.argv[3]
    nm = sys.argv[4] if len(sys.argv) > 4 else "nm"
    readelf = sys.argv[5] if len(sys.argv) > 5 else "readelf"

    image = bytearray(open(binary, "rb").read())
    syms = symbols(elf, nm)

    entry = syms.get("_start")
    if entry is None:
        sys.exit(f"{elf}: no _start symbol -- is crt0.c linked in?")
    if entry != LINK_ADDR:
        sys.exit(f"{elf}: _start is at {entry:#x}, expected {LINK_ADDR:#x}. "
                 "The .text.start section has to come first in app.ld.")

    bss = max(0, syms.get("_bss_end", 0) - syms.get("_bss_start", 0))

    relocs = relocations(elf, readelf, len(image))

    # The image has to end exactly where .bss begins.
    #
    # The kernel loads a program by copying image_size bytes and then zeroing
    # bss_size bytes immediately after them. That is only correct if the
    # program's .bss really does start at image_size. It used to not: the
    # relocation list was appended to the image, so image_size ran past the
    # start of .bss, the copied relocation list landed on top of the
    # program's variables, and every program started with garbage in them
    # instead of zeroes. A desktop built on that opened a window nobody
    # asked for, with a width of four characters, at boot.
    #
    # So: pad the image out to where the linker put .bss, and put the
    # relocation list after it in the file rather than inside the image.
    # The loader reads relocations straight out of the file, so they never
    # needed to be in the copied part at all.
    bss_start = syms.get("_bss_start", LINK_ADDR + len(image))
    pad_to = bss_start - LINK_ADDR
    if pad_to < len(image):
        sys.exit(f"{elf}: .bss starts at {bss_start:#x}, before the end of "
                 f"the image -- the link script put a section out of order")
    while len(image) < pad_to:
        image.append(0)

    image_size = len(image)
    checksum = sum(image) & 0xFFFFFFFF

    reloc_off = image_size
    for off in relocs:
        image += struct.pack("<I", off)

    if image_size + bss > MAX_IMAGE:
        sys.exit(f"{out}: {image_size + bss} bytes of image+bss, "
                 f"the limit is {MAX_IMAGE}")

    header = struct.pack("<IHBBIIIIIIII",
                         MAGIC, FMT, ARCH_X86_32, 0, LINK_ADDR, entry,
                         image_size, bss, checksum, API_MIN,
                         len(relocs), reloc_off)
    assert len(header) == HDR_SIZE, len(header)

    with open(out, "wb") as f:
        f.write(header)
        f.write(image)

    name = out.split("/")[-1]
    print(f"  {name:<14} {image_size:5d} B image + {bss:5d} B bss, "
          f"{len(relocs):3d} relocations")


if __name__ == "__main__":
    main()
