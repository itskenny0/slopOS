#!/usr/bin/env python3
"""addapp.py -- put your own files and programs into a PicoOS image.

    python3 tools/addapp.py picoos-hybrid.img mygame.pico
    python3 tools/addapp.py picoos-hybrid.img --esp-only mygame.pico big.wad
    python3 tools/addapp.py picoos.img notes.txt hello2.pico
    python3 tools/addapp.py picoos.img --list
    python3 tools/addapp.py picoos.img --remove mygame.pico

No rebuild, no compiler, no mtools -- just Python and the image file. The
files are appended to the image in a small archive that sits directly behind
the kernel, and the boot sector is told to read that far. PicoOS finds the
archive at the first sector boundary after its own end and mounts it into the
ramdisk, so your program shows up in `ls` and `progs` and starts with
`start <name>` like any other.

On the hybrid image the same archive also has to reach the UEFI side, where
the kernel is a file called KERNEL.BIN on the EFI system partition. This
script writes FAT16 itself rather than shelling out to mtools, so it runs on
a bare Python install.

With --esp-only (hybrid images only) the files go to the EFI partition's
copy of the kernel and nowhere else. The BIOS side keeps booting exactly
as before -- without the new files: the 512-byte boot sector cannot load
more than a megabyte, so a 4 MB WAD behind the raw kernel would break it.
UEFI boots get the files; BIOS boots do not. --list reads the raw side,
so it will not show files added this way.

Layout of the archive, version 2:

    u32 'PAPP'  u32 total size  u32 file count  u32 version
    count x { char name[16]; u32 stored size; u32 offset from the archive
              start; u32 flags; u32 original size }
    the file data, each padded to a 4-byte boundary

Programs go in compressed -- flag bit 0 -- and the kernel keeps them that
way until something runs them. Text files go in as they are, because `cat`
on a compressed file would have to unpack it into the heap first and a
readme is not worth that. Anything the packer cannot actually shrink is
stored raw, with the two sizes equal to say so.

Version 1 archives, which had 24-byte entries and no compression, are still
read correctly here and by the kernel.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lzss import compress, decompress

SECTOR = 512
NAMELEN = 16                 # FS_NAMELEN in the kernel
MAGIC = b'PAPP'
MAXFILES = 64
ESP_LBA = int(os.environ.get('PICO_ESP_LBA', '2048'))  # mkhybrid's EFI spot
VERSION = 2
ENTSZ = 32                   # bytes per directory entry in version 2
ENTSZ_V1 = 24
PACKED = 1                   # entry flag: the data is an LZSS stream


def die(msg):
    sys.exit("addapp: " + msg)


# ---------------------------------------------------------------- archive --

def should_pack(name, data):
    """Programs are worth compressing; small text files are not worth the
    loss of being readable in place."""
    return name.endswith('.pico') and len(data) > 64


def pack(files):
    """files: list of (name, bytes) -> the archive as bytes."""
    if len(files) > MAXFILES:
        die(f"{len(files)} files, but the kernel mounts at most {MAXFILES}")

    head = 16 + ENTSZ * len(files)
    blob, table, off = b'', b'', head

    for name, data in files:
        stored, flags = data, 0
        if should_pack(name, data):
            squeezed = compress(data)
            if len(squeezed) < len(data):
                assert decompress(squeezed, len(data)) == data, \
                    f"round trip failed for {name}"
                stored, flags = squeezed, PACKED

        nb = name.encode('ascii', 'replace')[:NAMELEN - 1]
        table += (nb.ljust(NAMELEN, b'\0')
                  + struct.pack('<IIII', len(stored), off, flags, len(data)))
        pad = (-len(stored)) % 4
        blob += stored + b'\0' * pad
        off += len(stored) + pad

    total = head + len(blob)
    return (struct.pack('<4sIII', MAGIC, total, len(files), VERSION)
            + table + blob)


def unpack(buf):
    """Read an archive back out, unpacking anything that is compressed, or
    return [] if there is not one here."""
    if len(buf) < 16 or buf[:4] != MAGIC:
        return []
    total, count, version = struct.unpack_from('<III', buf, 4)
    if total > len(buf) or count > MAXFILES:
        return []

    entsz = ENTSZ if version >= 2 else ENTSZ_V1
    out = []
    for i in range(count):
        base = 16 + entsz * i
        if version >= 2:
            name, size, off, flags, orig = struct.unpack_from('<16sIIII',
                                                              buf, base)
        else:
            name, size, off = struct.unpack_from('<16sII', buf, base)
            flags, orig = 0, size

        data = bytes(buf[off:off + size])
        if flags & PACKED:
            try:
                data = decompress(data, orig)
            except ValueError:
                continue        # a corrupt entry costs that file, not the run
        out.append((name.rstrip(b'\0').decode('ascii', 'replace'), data))
    return out


# ------------------------------------------------------------------- FAT --
#
# Just enough FAT to replace one file with a bigger one. Both FAT16 and
# FAT32 are handled: the EFI partition is FAT32, because that is what fussy
# firmware wants before it will call a USB stick bootable, while images built
# with earlier 0.3 builds are FAT16. FAT12 is not, and is rejected rather
# than guessed at.
# (old note) mkhybrid.py always
# makes a 16 MiB FAT16 volume, so there is no FAT12 or FAT32 case to handle;
# anything else is rejected rather than guessed at.

class Fat:
    def __init__(self, img, part_off):
        self.img, self.base = img, part_off
        b = img[part_off:part_off + 512]
        self.bps = struct.unpack_from('<H', b, 11)[0]
        self.spc = b[13]
        self.rsvd = struct.unpack_from('<H', b, 14)[0]
        self.nfat = b[16]
        self.rootent = struct.unpack_from('<H', b, 17)[0]
        self.fatsz = struct.unpack_from('<H', b, 22)[0] \
            or struct.unpack_from('<I', b, 36)[0]
        tot16 = struct.unpack_from('<H', b, 19)[0]
        tot32 = struct.unpack_from('<I', b, 32)[0]
        self.total = tot16 or tot32
        if self.bps != 512 or not self.fatsz or not self.spc:
            die("the EFI partition is not a FAT volume this tool understands")

        # rootent == 0 is the definition of FAT32: its root directory is an
        # ordinary cluster chain instead of a fixed-size area.
        self.f32 = (self.rootent == 0)
        self.width = 4 if self.f32 else 2
        self.rootclus = struct.unpack_from('<I', b, 44)[0] if self.f32 else 0
        self.eoc = 0x0FFFFFF8 if self.f32 else 0xFFF8

        self.fat0 = part_off + self.rsvd * self.bps
        self.rootoff = self.fat0 + self.nfat * self.fatsz * self.bps
        rootsz = (self.rootent * 32 + self.bps - 1) // self.bps * self.bps
        self.dataoff = self.rootoff + rootsz
        self.nclus = (self.total - (self.dataoff - part_off) // self.bps) \
            // self.spc + 2

    # --- the allocation table -------------------------------------------
    def get(self, c):
        o = self.fat0 + c * self.width
        if self.f32:
            return struct.unpack_from('<I', self.img, o)[0] & 0x0FFFFFFF
        return struct.unpack_from('<H', self.img, o)[0]

    def set(self, c, v):
        for f in range(self.nfat):
            o = self.fat0 + f * self.fatsz * self.bps + c * self.width
            if self.f32:
                # the top four bits of a FAT32 entry are reserved, not ours
                keep = struct.unpack_from('<I', self.img, o)[0] & 0xF0000000
                struct.pack_into('<I', self.img, o, keep | (v & 0x0FFFFFFF))
            else:
                struct.pack_into('<H', self.img, o, v)

    def chain(self, first):
        out, c = [], first
        while 2 <= c < self.eoc and len(out) < self.nclus:
            out.append(c)
            c = self.get(c)
        return out

    def free_chain(self, first):
        for c in self.chain(first):
            self.set(c, 0)

    def alloc(self, n):
        got = []
        for c in range(2, self.nclus):
            if self.get(c) == 0:
                got.append(c)
                if len(got) == n:
                    break
        if len(got) < n:
            die("the EFI partition is full")
        end = 0x0FFFFFFF if self.f32 else 0xFFFF
        for i, c in enumerate(got):
            self.set(c, end if i == n - 1 else got[i + 1])
        return got

    def clus_off(self, c):
        return self.dataoff + (c - 2) * self.spc * self.bps

    # --- directories -----------------------------------------------------
    @staticmethod
    def name83(name):
        stem, _, ext = name.upper().partition('.')
        return stem[:8].ljust(8).encode() + ext[:3].ljust(3).encode()

    def dir_entries(self, off, count):
        for i in range(count):
            yield off + i * 32

    def find(self, path):
        """Return the byte offset of the directory entry for `path`."""
        parts = path.strip('/').split('/')
        if self.f32:
            cl = self.chain(self.rootclus)
            off = self.clus_off(cl[0])
            count = len(cl) * self.spc * self.bps // 32
        else:
            off, count = self.rootoff, self.rootent
        for depth, part in enumerate(parts):
            want = self.name83(part)
            hit = None
            for e in self.dir_entries(off, count):
                if self.img[e] in (0x00, 0xE5):
                    continue
                attr = self.img[e + 11]
                if attr & 0x0F == 0x0F:                  # long file name
                    continue
                if attr & 0x08:                          # the volume label,
                    continue                             # which shares a name
                if bytes(self.img[e:e + 11]) != want:
                    continue
                is_dir = bool(attr & 0x10)
                if is_dir != (depth < len(parts) - 1):
                    continue
                hit = e
                break
            if hit is None:
                die(f"{path} is not on the EFI partition")
            if depth == len(parts) - 1:
                return hit
            cl = self.chain(self.ent_clus(hit))
            off = self.clus_off(cl[0])
            count = len(cl) * self.spc * self.bps // 32
        die("unreachable")

    def ent_clus(self, e):
        """A directory entry keeps the cluster number in two halves."""
        lo = struct.unpack_from('<H', self.img, e + 26)[0]
        hi = struct.unpack_from('<H', self.img, e + 20)[0]
        return (hi << 16) | lo

    def read_file(self, path):
        e = self.find(path)
        size = struct.unpack_from('<I', self.img, e + 28)[0]
        out = b''
        for c in self.chain(self.ent_clus(e)):
            o = self.clus_off(c)
            out += bytes(self.img[o:o + self.spc * self.bps])
        return out[:size]

    def write_file(self, path, data):
        e = self.find(path)
        self.free_chain(self.ent_clus(e))

        csize = self.spc * self.bps
        need = (len(data) + csize - 1) // csize
        clus = self.alloc(need)
        for i, c in enumerate(clus):
            chunk = data[i * csize:(i + 1) * csize].ljust(csize, b'\0')
            o = self.clus_off(c)
            self.img[o:o + csize] = chunk
        struct.pack_into('<H', self.img, e + 26, clus[0] & 0xFFFF)
        struct.pack_into('<H', self.img, e + 20, (clus[0] >> 16) & 0xFFFF)
        struct.pack_into('<I', self.img, e + 28, len(data))


# ------------------------------------------------------------------ image --

def load(path):
    img = bytearray(open(path, 'rb').read())
    if len(img) < 1024 or img[510:512] != b'\x55\xaa':
        die(f"{path} does not look like a PicoOS image")
    nsec = struct.unpack_from('<H', img, 436)[0]
    konly = struct.unpack_from('<H', img, 438)[0]
    if not 1 <= konly <= nsec:
        die("this image predates addapp; rebuild it or use a newer one")

    # Three layouts to tell apart: the plain BIOS floppy, the MBR hybrid, and
    # the GPT UEFI-only disk. The GPT one has no raw kernel behind the boot
    # sector -- everything lives on the EFI partition.
    gpt = bytes(img[SECTOR:SECTOR + 8]) == b'EFI PART'
    off = ESP_LBA * SECTOR
    has_esp = len(img) > off + 512 and (
        bytes(img[off + 54:off + 59]) == b'FAT16' or
        bytes(img[off + 82:off + 87]) == b'FAT32')
    return img, nsec, konly, has_esp, gpt


def save(path, img, kernel, archive, konly, has_esp, gpt, esp_only=False):
    body = kernel + archive
    body += b'\0' * ((-len(body)) % SECTOR)
    nsec = len(body) // SECTOR

    if not gpt and not esp_only:
        limit = (ESP_LBA - 1) if has_esp else (len(img) // SECTOR - 1)
        if nsec > limit:
            die(f"no room: the kernel plus your files needs {nsec} sectors, "
                f"but only {limit} fit before the next thing on the disk")
        img[SECTOR:SECTOR + len(body)] = body
        struct.pack_into('<H', img, 436, nsec)   # what the boot sector reads
    struct.pack_into('<H', img, 438, konly)      # unchanged: where we end

    if has_esp:
        Fat(img, ESP_LBA * SECTOR).write_file('/picoos/kernel.bin',
                                              bytes(kernel + archive))

    tmp = path + '.new'
    open(tmp, 'wb').write(bytes(img))
    os.replace(tmp, path)
    return nsec


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)

    image = args[0]
    rest = args[1:]
    if not os.path.exists(image):
        die(f"no such image: {image}")
    if image.lower().endswith('.iso'):
        die("the .iso is read-only by nature -- add your files to "
            "picoos.img or picoos-hybrid.img instead")

    img, nsec, konly, has_esp, gpt = load(image)
    esp_only = '--esp-only' in rest
    rest = [a for a in rest if a != '--esp-only']
    if esp_only and (gpt or not has_esp):
        die("--esp-only needs a hybrid image with an EFI partition")
    if gpt or esp_only:
        # no raw kernel on this disk (GPT); or the raw side stays frozen
        # and the ESP copy is the only one that grows (--esp-only)
        blob = Fat(img, ESP_LBA * SECTOR).read_file('/picoos/kernel.bin')
    else:
        blob = bytes(img[SECTOR:SECTOR + nsec * SECTOR])
    kernel = blob[:konly * SECTOR]
    files = unpack(blob[konly * SECTOR:])

    kind = 'GPT UEFI-only' if gpt else \
           'hybrid BIOS+UEFI' if has_esp else 'BIOS'
    if not rest or rest[0] in ('--list', '-l'):
        print(f"{image}: {kind} image, "
              f"kernel {konly * SECTOR // 1024} KB")
        if not files:
            print("  no files added yet")
        else:
            for n, d in files:
                print(f"  {n:<16} {len(d):>7} bytes")
            print(f"  {len(files)} file(s), "
                  f"{sum(len(d) for _, d in files)} bytes")
        return

    if rest[0] in ('--remove', '-r', '--rm'):
        if len(rest) < 2:
            die("--remove needs a name")
        gone = [n for n in rest[1:] if n in dict(files)]
        files = [(n, d) for n, d in files if n not in rest[1:]]
        if not gone:
            die("none of those are in the image (try --list)")
        for n in gone:
            print(f"  removed {n}")
    else:
        for path in rest:
            if not os.path.exists(path):
                die(f"no such file: {path}")
            name = os.path.basename(path)
            if len(name) >= NAMELEN:
                die(f"'{name}' is too long -- {NAMELEN - 1} characters max")
            data = open(path, 'rb').read()
            if name.endswith('.pico') and data[:4] != b'PICO':
                die(f"{name} is not a .pico program (bad magic)")
            files = [(n, d) for n, d in files if n != name]
            files.append((name, data))
            if should_pack(name, data):
                sq = compress(data)
                if len(sq) < len(data):
                    print(f"  added {name}  ({len(data)} bytes, "
                          f"{len(sq)} packed)")
                    continue
            print(f"  added {name}  ({len(data)} bytes)")

    archive = pack(files) if files else b''
    nsec = save(image, img, kernel, archive, konly, has_esp, gpt, esp_only)
    if esp_only:
        print(f"  ESP only: the BIOS side is untouched and will not see these files")
    if gpt:
        print(f"  {image}: EFI copy updated, "
              f"kernel {konly} + archive {nsec - konly} sectors")
    else:
        print(f"  {image}: kernel {konly} + archive "
              f"{nsec - konly} = {nsec} sectors"
              + (", EFI copy updated" if has_esp else ""))


if __name__ == '__main__':
    main()
