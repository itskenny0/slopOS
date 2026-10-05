#!/usr/bin/env python3
"""Build the hybrid PicoOS image.

Layout, so that one file boots on a 486 and on a 2024 laptop:

    LBA 0            MBR: BIOS boot code + partition table
    LBA 1 .. N       the raw kernel, read by the BIOS boot sector
    LBA 2048 ..      FAT EFI system partition (FAT16 on the small image,
                     FAT32 on the big one) holding /EFI/BOOT/BOOTX64.EFI
                     and /picoos/kernel.bin

Old machines run the boot sector and never look at the partition. UEFI
machines ignore the boot sector, find the ESP and run BOOTX64.EFI.
"""
import os
import subprocess
import sys

boot_p, kern_p, efi_p, out_p = sys.argv[1:5]
boot_elf = sys.argv[5] if len(sys.argv) > 5 else None

SECTOR = 512
ESP_LBA = int(os.environ.get('PICO_ESP_LBA', '2048'))  # 1 MiB in, conventional
# 64 MiB. The size is not about the 50 KB we put in it: it is the smallest
# volume that is comfortably a *real* FAT32, and FAT32 is what a lot of UEFI
# firmware insists on before it will treat a USB stick as bootable at all.
# A FAT16 ESP is legal and works in QEMU, and is exactly the kind of thing
# that boots on every machine you own and none of the ones you do not.
# Both of these can be overridden from the environment, which is how the
# makefile builds the small image. Everything else about the layout -- the
# gap before the partition, the alignment, where the kernel hides -- is
# the same either way, so a smaller ESP gives a smaller stick to write and
# nothing else changes.
ESP_SECTORS = int(os.environ.get('PICO_ESP_SECTORS', '131072'))
ESP_FAT     = int(os.environ.get('PICO_ESP_FAT', '32'))
TOTAL = (ESP_LBA + ESP_SECTORS) * SECTOR

boot = bytearray(open(boot_p, 'rb').read())
kern = bytearray(open(kern_p, 'rb').read())

if len(boot) != 512 or boot[510:512] != b'\x55\xaa':
    sys.exit("bad boot sector")

# ---- patch the sector count into the boot sector -----------------------
sym = {'kernel_sectors': 500, 'kernel_only': 502}
if boot_elf:
    nm = subprocess.run(['nm', boot_elf], capture_output=True, text=True).stdout
    for line in nm.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] in sym:
            sym[p[2]] = int(p[0], 16) - 0x7C00
ksectors = (len(kern) + SECTOR - 1) // SECTOR
for name in ('kernel_sectors', 'kernel_only'):
    o = sym[name]
    boot[o] = ksectors & 0xFF
    boot[o + 1] = (ksectors >> 8) & 0xFF

# The kernel on the ESP is padded to a whole sector too, so that the app
# archive lands at the same offset there as it does on the raw disk.
kern += b'\x00' * (ksectors * SECTOR - len(kern))
open('/tmp/picoos_kern_padded.bin', 'wb').write(bytes(kern))

if 1 + ksectors > ESP_LBA:
    sys.exit("kernel would overlap the EFI partition")

# ---- build the EFI system partition ------------------------------------
if ESP_FAT == 16:
    # pure python, no mtools needed
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from mkesp import build_esp
    esp_data = build_esp(ESP_SECTORS,
                         [("EFI/BOOT/BOOTX64.EFI", open(efi_p, 'rb').read()),
                          ("picoos/kernel.bin", bytes(kern))])
else:
    # FAT32 still goes through the system tools
    esp = '/tmp/picoos_esp.img'
    subprocess.run(['dd', 'if=/dev/zero', f'of={esp}', 'bs=512',
                    f'count={ESP_SECTORS}'], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
    # -s 1 forces 512-byte clusters, so 64 MiB gives ~131000 of them -- well
    # past the 65525 floor below which a volume is not a valid FAT32 and
    # firmware is entitled to reject it.
    try:
        subprocess.run(['mkfs.fat', '-F', str(ESP_FAT), '-s', '1', '-n',
                        'PICOOS', esp],
                       check=True, stdout=subprocess.DEVNULL)
    except FileNotFoundError:
        sys.exit("mkhybrid: a FAT32 ESP needs mkfs.fat/mtools; "
                 "the FAT16 small image builds without them")
    subprocess.run(['mmd', '-i', esp, '::/EFI'], check=True, env=env)
    subprocess.run(['mmd', '-i', esp, '::/EFI/BOOT'], check=True, env=env)
    subprocess.run(['mmd', '-i', esp, '::/picoos'], check=True, env=env)
    subprocess.run(['mcopy', '-i', esp, efi_p, '::/EFI/BOOT/BOOTX64.EFI'],
                   check=True, env=env)
    subprocess.run(['mcopy', '-i', esp, '/tmp/picoos_kern_padded.bin',
                    '::/picoos/kernel.bin'],
                   check=True, env=env)
    esp_data = open(esp, 'rb').read()

# ---- assemble the whole image ------------------------------------------
img = bytearray(TOTAL)
img[0:512] = boot
img[512:512 + len(kern)] = kern
img[ESP_LBA * SECTOR: ESP_LBA * SECTOR + len(esp_data)] = esp_data


# a non-zero MBR disk signature at 0x1B8, or UEFI will not offer the disk
img[0x1B8:0x1BC] = (0x50494330).to_bytes(4, 'little')
img[0x1BC:0x1BE] = b'\x00\x00'


def chs(lba):
    spt, heads = 63, 255
    c, h, s = lba // (spt * heads), (lba // spt) % heads, lba % spt + 1
    if c > 1023:
        c, h, s = 1023, 254, 63
    return bytes([h, ((c >> 2) & 0xC0) | s, c & 0xFF])


# partition 1: the EFI system partition (type 0xEF), flagged bootable so
# stubborn BIOSes are happy too
entry = (b'\x80' + chs(ESP_LBA) + b'\xEF' + chs(ESP_LBA + ESP_SECTORS - 1)
         + ESP_LBA.to_bytes(4, 'little') + ESP_SECTORS.to_bytes(4, 'little'))
img[446:462] = entry

open(out_p, 'wb').write(bytes(img))
if ESP_FAT != 16:
    os.remove(esp)

print(f"hybrid image : {out_p}")
print(f"  boot sector: BIOS, reads {ksectors} kernel sectors from LBA 1")
print(f"  ESP        : FAT{ESP_FAT} at LBA {ESP_LBA}, "
      f"/EFI/BOOT/BOOTX64.EFI ({os.path.getsize(efi_p)} bytes)")
print(f"  total size : {TOTAL // 1024 // 1024} MB")
