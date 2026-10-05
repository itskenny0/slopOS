#!/usr/bin/env python3
"""Patch the kernel's sector count into the ISO's MBR boot sector.

The MBR has to know how many sectors to load, and it has to be told after
the kernel has been built rather than being assembled with a number in it
that goes stale. The offset comes out of the symbol table, so it keeps
working when the assembly above it moves -- which it will.

    mkisombr.py isohbr.bin isohbr.elf kernel.bin
"""
import subprocess, sys, os

bin_p, elf_p, kern_p = sys.argv[1:4]
SECTOR = 512

ksectors = (os.path.getsize(kern_p) + SECTOR - 1) // SECTOR

nm = subprocess.run(['nm', elf_p], capture_output=True, text=True).stdout
off = None
for line in nm.splitlines():
    p = line.split()
    if len(p) == 3 and p[2] == 'kernel_sectors':
        off = int(p[0], 16) - 0x7C00
if off is None:
    sys.exit("mkisombr: no kernel_sectors symbol in the elf")

d = bytearray(open(bin_p, 'rb').read())
if len(d) > 512:
    sys.exit("mkisombr: the mbr is %d bytes, it has to fit in 512" % len(d))
if len(d) < 512:
    d += bytearray(512 - len(d))

if off + 2 > 432:
    sys.exit("mkisombr: the sector count at %d runs into xorrisofs' patch area" % off)

d[off]     = ksectors & 0xFF
d[off + 1] = (ksectors >> 8) & 0xFF
d[510] = 0x55
d[511] = 0xAA
open(bin_p, 'wb').write(bytes(d))
print("  iso mbr      : %d bytes, kernel is %d sectors" % (len(d), ksectors))
