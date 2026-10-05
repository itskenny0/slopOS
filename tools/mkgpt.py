#!/usr/bin/env python3
"""Build picoos-uefi.img -- a GPT-partitioned, UEFI-only PicoOS disk.

Why a third image. `picoos-hybrid.img` is partitioned with an MBR, which is
what lets the same file also boot a 486. Plenty of modern firmware does not
care and boots it anyway, but some -- particularly recent laptops and
tablets that shipped without a CSM at all -- will only offer a removable
disk as a boot device when it carries a real GPT. There is no way to have
both on one disk without a hybrid MBR, which is a well known source of
firmware that boots neither. So this is a separate file with one job.

Layout:

    LBA 0            protective MBR, one 0xEE partition covering the disk
    LBA 1            GPT header
    LBA 2 .. 33      128 partition entries
    LBA 2048 ..      FAT32 EFI system partition:
                        /EFI/BOOT/BOOTX64.EFI
                        /picoos/kernel.bin
    last 33 sectors  backup partition entries + backup GPT header

There is no boot sector and no raw kernel behind it: under UEFI the loader
reads the kernel as a file off the EFI partition, so none of that is needed.
"""
import os
import struct
import subprocess
import sys
import uuid
import zlib

efi_p, kern_p, out_p = sys.argv[1:4]

SECTOR = 512
ESP_LBA = 2048
ESP_SECTORS = 131072                 # 64 MiB, comfortably a real FAT32
TOTAL_SECTORS = ESP_LBA + ESP_SECTORS + 2048
LAST = TOTAL_SECTORS - 1

ESP_TYPE = uuid.UUID('C12A7328-F81F-11D2-BA4B-00A0C93EC93B')


def guid(u):
    """GPT stores the first three fields of a GUID little-endian."""
    b = u.bytes
    return (b[3::-1] + b[5:3:-1] + b[7:5:-1] + b[8:])


# ---- the EFI system partition -----------------------------------------
esp = '/tmp/picoos_gpt_esp.img'
subprocess.run(['dd', 'if=/dev/zero', f'of={esp}', 'bs=512',
                f'count={ESP_SECTORS}'], check=True,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
subprocess.run(['mkfs.fat', '-F', '32', '-s', '1', '-n', 'PICOOS', esp],
               check=True, stdout=subprocess.DEVNULL)
for d in ('::/EFI', '::/EFI/BOOT', '::/picoos'):
    subprocess.run(['mmd', '-i', esp, d], check=True, env=env)
subprocess.run(['mcopy', '-i', esp, efi_p, '::/EFI/BOOT/BOOTX64.EFI'],
               check=True, env=env)

kern = open(kern_p, 'rb').read()
kern += b'\0' * ((-len(kern)) % SECTOR)
open('/tmp/picoos_gpt_kern.bin', 'wb').write(kern)
subprocess.run(['mcopy', '-i', esp, '/tmp/picoos_gpt_kern.bin',
                '::/picoos/kernel.bin'], check=True, env=env)

img = bytearray(TOTAL_SECTORS * SECTOR)
img[ESP_LBA * SECTOR:(ESP_LBA + ESP_SECTORS) * SECTOR] = open(esp, 'rb').read()

# ---- protective MBR ----------------------------------------------------
# One partition of type 0xEE spanning the disk, so that anything that only
# understands MBR sees a disk that is fully used and does not offer to
# "repair" it. It is deliberately not marked bootable: this disk is UEFI
# only and pretending otherwise just gets it tried and rejected.
mbr = bytearray(SECTOR)
mbr[446:462] = (b'\x00'                                  # not bootable
                + b'\x00\x02\x00'                        # CHS start 0/0/2
                + b'\xEE'                                # GPT protective
                + b'\xFF\xFF\xFF'                        # CHS end: maxed out
                + (1).to_bytes(4, 'little')
                + min(LAST, 0xFFFFFFFF).to_bytes(4, 'little'))
mbr[510:512] = b'\x55\xAA'

# The same two words the hybrid image keeps here: how many sectors of kernel
# there are, and how many of those are the kernel proper. Nothing boots from
# this MBR, but tools/addapp.py reads them to find where it may append.
ksectors = len(kern) // SECTOR
struct.pack_into('<HH', mbr, 436, ksectors, ksectors)

img[0:SECTOR] = mbr

# ---- partition entries -------------------------------------------------
entries = bytearray(128 * 128)
name = 'EFI System Partition'.encode('utf-16-le').ljust(72, b'\0')
entries[0:128] = (guid(ESP_TYPE)
                  + guid(uuid.uuid4())
                  + struct.pack('<QQQ', ESP_LBA, ESP_LBA + ESP_SECTORS - 1, 0)
                  + name)
entries_crc = zlib.crc32(entries) & 0xFFFFFFFF

disk_guid = guid(uuid.uuid4())


def header(my_lba, alt_lba, entry_lba):
    h = bytearray(92)
    h[0:8] = b'EFI PART'
    struct.pack_into('<II', h, 8, 0x00010000, 92)        # revision, size
    struct.pack_into('<I', h, 16, 0)                     # header crc, later
    struct.pack_into('<I', h, 20, 0)                     # reserved
    struct.pack_into('<QQ', h, 24, my_lba, alt_lba)
    struct.pack_into('<QQ', h, 40, 34, LAST - 33)        # first/last usable
    h[56:72] = disk_guid
    struct.pack_into('<Q', h, 72, entry_lba)
    struct.pack_into('<III', h, 80, 128, 128, entries_crc)
    struct.pack_into('<I', h, 16, zlib.crc32(h) & 0xFFFFFFFF)
    return bytes(h)


img[1 * SECTOR:1 * SECTOR + 92] = header(1, LAST, 2)
img[2 * SECTOR:2 * SECTOR + len(entries)] = entries

backup_entries_lba = LAST - 32
img[backup_entries_lba * SECTOR:
    backup_entries_lba * SECTOR + len(entries)] = entries
img[LAST * SECTOR:LAST * SECTOR + 92] = header(LAST, 1, backup_entries_lba)

open(out_p, 'wb').write(bytes(img))

print(f"uefi image  : {out_p}")
print(f"  scheme     : GPT, protective MBR, no BIOS boot path")
print(f"  ESP        : FAT32 at LBA {ESP_LBA}, "
      f"/EFI/BOOT/BOOTX64.EFI ({os.path.getsize(efi_p)} bytes)")
print(f"  total size : {TOTAL_SECTORS * SECTOR // (1024 * 1024)} MB")
