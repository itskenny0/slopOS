#!/usr/bin/env python3
"""Build the PicoOS disk image.

Glues the 512-byte boot sector and the raw kernel together, patches the
sector count the bootloader has to read, and writes an MBR partition table
so BIOSes are willing to boot the thing off a USB stick.
"""
import subprocess
import sys

boot_p, kern_p, out_p = sys.argv[1], sys.argv[2], sys.argv[3]
boot_elf = sys.argv[4] if len(sys.argv) > 4 else None

boot = bytearray(open(boot_p, 'rb').read())
kern = bytearray(open(kern_p, 'rb').read())

if len(boot) != 512:
    sys.exit(f"boot sector must be exactly 512 bytes, got {len(boot)}")
if boot[510:512] != b'\x55\xaa':
    sys.exit("boot sector is missing the 0xAA55 signature")

# ---- where do the two size words live? ---------------------------------
sym = {'kernel_sectors': 500, 'kernel_only': 502}
if boot_elf:
    nm = subprocess.run(['nm', boot_elf], capture_output=True, text=True).stdout
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in sym:
            sym[parts[2]] = int(parts[0], 16) - 0x7C00
off = sym['kernel_sectors']
if not all(0 <= o <= 444 for o in sym.values()):
    sys.exit(f"size words at a bad offset ({sym}) -- boot code too big?")

sectors = (len(kern) + 511) // 512
if sectors == 0:
    sys.exit("empty kernel")
for name in ('kernel_sectors', 'kernel_only'):
    o = sym[name]
    boot[o]     = sectors & 0xFF
    boot[o + 1] = (sectors >> 8) & 0xFF

# ---- pad the kernel to whole sectors -----------------------------------
kern += b'\x00' * (sectors * 512 - len(kern))

FLOPPY = 1474560                      # 1.44 MB, so it also fits a real floppy
img = boot + kern
if len(img) > FLOPPY:
    sys.exit("image is bigger than a 1.44 MB floppy")
img += b'\x00' * (FLOPPY - len(img))


# a non-zero MBR disk signature at 0x1B8, or UEFI will not offer the disk
img[0x1B8:0x1BC] = (0x50494330).to_bytes(4, 'little')
img[0x1BC:0x1BE] = b'\x00\x00'

# ---- MBR partition table (entry 1 covers the whole image) --------------
total = FLOPPY // 512


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
entry += b'\x01'                      # type: FAT12 (cosmetic, we're raw)
entry += chs(total - 1)               # last sector
entry += (0).to_bytes(4, 'little')    # start LBA
entry += total.to_bytes(4, 'little')  # length in sectors
assert len(entry) == 16

img[446:462] = entry

open(out_p, 'wb').write(bytes(img))

print(f"boot sector : 512 bytes (kernel_sectors patched at offset {off})")
print(f"kernel      : {len(kern)} bytes ({sectors} sectors)")
print(f"image       : {out_p} -- 1.44 MB, {512 + len(kern)} bytes in use")
