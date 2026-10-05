#!/usr/bin/env python3
"""Build a hard-disk-style PicoOS image with extra files appended.

Like mkimage.py, but the image grows to fit: the raw kernel is followed
by a PAPP archive (see addapp.py) holding extra programs and data files,
and the whole thing is padded to a megabyte boundary with an MBR
partition table so BIOSes boot it off a USB stick.

Made for the DOOM image: kernel + doom.pico + the 4 MB WAD do not fit
on a 1.44 MB floppy.

Usage:
    mkhd.py boot.bin kernel.bin out.img boot.elf name1=path1 [name2=path2 ...]
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from addapp import pack

if len(sys.argv) < 5:
    sys.exit("usage: mkhd.py boot.bin kernel.bin out.img boot.elf "
             "name1=path1 [name2=path2 ...]")

boot_p, kern_p, out_p, boot_elf = sys.argv[1:5]

boot = bytearray(open(boot_p, 'rb').read())
kern = bytearray(open(kern_p, 'rb').read())

if len(boot) != 512:
    sys.exit(f"boot sector must be exactly 512 bytes, got {len(boot)}")
if boot[510:512] != b'\x55\xaa':
    sys.exit("boot sector is missing the 0xAA55 signature")

files = []
for spec in sys.argv[5:]:
    if '=' not in spec:
        sys.exit(f"bad file spec {spec!r}, want name=path")
    name, path = spec.split('=', 1)
    files.append((name, open(path, 'rb').read()))

archive = pack(files)

# ---- where do the two size words live? (same as mkimage.py) ------------
sym = {'kernel_sectors': 500, 'kernel_only': 502}
if boot_elf and boot_elf != '-':
    nm = subprocess.run(['nm', boot_elf], capture_output=True, text=True).stdout
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in sym:
            sym[parts[2]] = int(parts[0], 16) - 0x7C00
if not all(0 <= o <= 444 for o in sym.values()):
    sys.exit(f"size words at a bad offset ({sym}) -- boot code too big?")

konly = (len(kern) + 511) // 512
body = bytes(kern) + archive
sectors = (len(body) + 511) // 512
if sectors > 0xFFFF:
    sys.exit("kernel plus archive is bigger than the loader can count")
for name, val in (('kernel_sectors', sectors), ('kernel_only', konly)):
    o = sym[name]
    boot[o] = val & 0xFF
    boot[o + 1] = (val >> 8) & 0xFF

body += b'\x00' * (sectors * 512 - len(body))

# ---- the image is the boot sector plus the body, at a MB boundary ------
img = bytes(boot) + body
total = (len(img) + 0xFFFFF) // 0x100000 * 0x100000 // 512
img += b'\x00' * (total * 512 - len(img))

img = bytearray(img)
img[0x1B8:0x1BC] = (0x50494330).to_bytes(4, 'little')
img[0x1BC:0x1BE] = b'\x00\x00'


def chs(lba):
    spt, heads = 63, 16
    c = lba // (spt * heads)
    h = (lba // spt) % heads
    s = lba % spt + 1
    if c > 1023:
        c, h, s = 1023, 15, 63
    return bytes([h, ((c >> 2) & 0xC0) | s, c & 0xFF])


entry = bytearray()
entry += b'\x80'                      # bootable
entry += chs(0)                       # first sector
entry += b'\x83'                      # type: Linux (cosmetic, we're raw)
entry += chs(total - 1)               # last sector
entry += (0).to_bytes(4, 'little')    # start LBA
entry += total.to_bytes(4, 'little')  # length in sectors
assert len(entry) == 16

img[446:462] = entry

open(out_p, 'wb').write(bytes(img))

print(f"kernel+archive: {len(body)} bytes ({sectors} sectors, "
      f"{len(archive)} of them archive)")
print(f"image       : {out_p} -- {len(img) // 1048576} MB, "
      f"{512 + len(body)} bytes in use")
