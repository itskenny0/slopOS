#!/usr/bin/env python3
"""Mark the partition that holds the bootloader as an EFI system partition.

-isohybrid-gpt-basdat gives the ISO a GPT, which is half the job: the
partition table a modern firmware wants to see. But every entry it writes
is "basic data", and firmware that goes looking for a partition of type
EFI System -- which is exactly what the specification tells it to do --
finds none, decides the disk has nothing to boot, and falls through to the
next option in its boot list. On a laptop that means Windows starts, ten
seconds after you asked it not to.

xorrisofs will take a type GUID, but only for the partition it makes for
the ISO as a whole, and that is not the one the bootloader lives on. So
find the partition that actually holds a FAT filesystem and mark that one.

    mkisoesp.py picoos.iso
"""
import struct, sys, zlib

iso = sys.argv[1]
d = bytearray(open(iso, 'rb').read())

# EFI System Partition, in the mixed-endian order a GPT stores it in
ESP_GUID = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")

if d[512:520] != b'EFI PART':
    sys.exit("mkisoesp: this image has no gpt")

entries_lba = struct.unpack_from('<Q', d, 512 + 0x48)[0]
count       = struct.unpack_from('<I', d, 512 + 0x50)[0]
esize       = struct.unpack_from('<I', d, 512 + 0x54)[0]
base        = entries_lba * 512


def holds_fat(off):
    """FAT32 names itself eleven bytes further in than FAT12 and 16 do."""
    return (bytes(d[off + 0x52:off + 0x5A]) == b'FAT32   ' or
            bytes(d[off + 0x36:off + 0x3B]) == b'FAT12' or
            bytes(d[off + 0x36:off + 0x3B]) == b'FAT16')


marked = 0
for i in range(count):
    e = base + i * esize
    if d[e:e + 16] == b'\x00' * 16:
        continue                                  # unused slot
    first = struct.unpack_from('<Q', d, e + 32)[0]
    if holds_fat(first * 512):
        d[e:e + 16] = ESP_GUID
        marked += 1

# Both checksums, array first and then the header over itself with its
# own field cleared: a firmware that checks them will not boot a table
# that does not add up.
struct.pack_into('<I', d, 512 + 0x0C, 0)
struct.pack_into('<I', d, 512 + 0x58,
                 zlib.crc32(bytes(d[base:base + count * esize])) & 0xFFFFFFFF)
struct.pack_into('<I', d, 512 + 0x0C,
                 zlib.crc32(bytes(d[512:512 + 92])) & 0xFFFFFFFF)

# While the file is open anyway: the partition an EFI system lives on is
# supposed to carry the active flag, and a fair number of firmwares -- the
# ones that treat a usb stick as a hard disk -- will not start a machine
# from a partition without it. xorrisofs leaves it clear.
for i in range(4):
    e = 446 + 16 * i
    if d[e + 4] == 0xEF:
        d[e] = 0x80

open(iso, 'wb').write(bytes(d))
print("  gpt esp      : %d partition(s) marked as efi system" % marked)
