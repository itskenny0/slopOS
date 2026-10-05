#!/usr/bin/env python3
"""Make a FAT16 EFI system partition image, with no mtools needed.

build_esp(sectors, files) -> bytes, where files is a list of
(fat_path, data) with fat_path like "EFI/BOOT/BOOTX64.EFI" (8.3 names
only, which is all an ESP needs) and data as bytes.

Layout: 512-byte sectors, one sector per cluster, two FATs, 512 root
entries. A directory is one cluster (32 entries); file data follows in
whole clusters. Timestamps are fixed so builds are reproducible.
"""
import struct
import sys

SECTOR = 512
DIR_ENTRIES_PER_CLUS = 16


def _shortname(path_part):
    part = path_part.upper()
    if "." in part:
        name, ext = part.split(".", 1)
    else:
        name, ext = part, ""
    if len(name) > 8 or len(ext) > 3:
        raise ValueError(f"{path_part!r} is not an 8.3 name")
    return name.ljust(8)[:8].encode("ascii") + ext.ljust(3)[:3].encode("ascii")


def _dir_entry(short, attr, cluster, size):
    # fixed date: 2026-09-21 12:00:00, so the image is reproducible
    date = ((2026 - 1980) << 9) | (9 << 5) | 21
    time = (12 << 11)
    return (short + struct.pack("<BBBHHHHHHHI", attr, 0, 0, time, date,
                                date, 0, time, date, cluster, size))


def build_esp(total_sectors, files):
    # ---- collect directories ----
    dirs = {"": []}      # path -> list of ("dir"|"file", name, payload)
    for fat_path, data in files:
        parts = fat_path.replace("\\", "/").strip("/").split("/")
        for i in range(len(parts) - 1):
            parent = "/".join(parts[:i])
            child = "/".join(parts[:i + 1])
            if child not in dirs:
                dirs[child] = []
                dirs[parent].append(("dir", parts[i], child))
        dirs["/".join(parts[:-1])].append(("file", parts[-1], data))

    # ---- assign one cluster per directory, chains for files ----
    order = [""] + sorted(d for d in dirs if d)
    clus = 2
    dir_clus = {}
    for d in order:
        if len(dirs[d]) + 2 > 32:
            raise ValueError(f"directory {d!r} holds more than 30 entries")
        dir_clus[d] = clus
        clus += 1
    file_runs = {}       # full path -> (first cluster, data)
    for d in order:
        for kind, name, payload in dirs[d]:
            if kind != "file":
                continue
            nclus = max(1, (len(payload) + SECTOR - 1) // SECTOR)
            file_runs[d + "/" + name if d else name] = (clus, payload)
            clus += nclus
    nclus = clus - 2

    # ---- FAT geometry ----
    reserved = 4
    root_sectors = 32    # 512 entries
    fat_sectors = 1
    for _ in range(8):   # converges immediately; loop for honesty
        data_start = reserved + 2 * fat_sectors + root_sectors
        data_clus = total_sectors - data_start
        if data_clus < nclus:
            raise ValueError("ESP too small for these files")
        fat_sectors = (data_clus * 2 + SECTOR - 1) // SECTOR
    if not 4085 <= data_clus <= 65524:
        raise ValueError(f"{data_clus} clusters is not a FAT16 volume")
    if nclus > data_clus:
        raise ValueError("ESP too small for these files")

    img = bytearray(total_sectors * SECTOR)

    # ---- boot sector ----
    boot = bytearray(SECTOR)
    boot[0:3] = b"\xeb\x3c\x90"
    boot[3:11] = b"PICOOS  "
    struct.pack_into("<HBHBHHBHHHII", boot, 11,
                     SECTOR, 1, reserved, 2, 512,
                     0 if total_sectors >= 65536 else total_sectors, 0xF8,
                     fat_sectors, 63, 255, 0,
                     total_sectors if total_sectors >= 65536 else 0)
    if total_sectors >= 65536:
        struct.pack_into("<H", boot, 19, 0)
        struct.pack_into("<I", boot, 32, total_sectors)
    else:
        struct.pack_into("<H", boot, 19, total_sectors)
    boot[36] = 0x80
    boot[38] = 0x29
    struct.pack_into("<I", boot, 39, 0x50494330)
    boot[43:54] = b"PICOOS     "
    boot[54:62] = b"FAT16   "
    boot[510:512] = b"\x55\xaa"
    img[0:SECTOR] = boot

    # ---- FATs ----
    fat = bytearray(fat_sectors * SECTOR)
    struct.pack_into("<HH", fat, 0, 0xFFF8, 0xFFFF)
    nxt = {}
    for d in order:
        c = dir_clus[d]
        nxt[c] = 0xFFFF
    for _, (first, payload) in file_runs.items():
        n = max(1, (len(payload) + SECTOR - 1) // SECTOR)
        for i in range(n):
            nxt[first + i] = 0xFFFF if i == n - 1 else first + i + 1
    for c, v in nxt.items():
        struct.pack_into("<H", fat, c * 2, v)
    for i in range(2):
        off = (reserved + i * fat_sectors) * SECTOR
        img[off:off + len(fat)] = fat

    # ---- root directory + subdirectories + file data ----
    def clus_off(c):
        return (data_start + c - 2) * SECTOR

    root = bytearray(root_sectors * SECTOR)
    root[0:32] = _dir_entry(b"PICOOS     ", 0x08, 0, 0)   # volume label
    pos = {"": 32}

    def put(parent_off, area, ent):
        area[parent_off:parent_off + 32] = ent

    for d in order:
        area = root if d == "" else bytearray(SECTOR)
        if d != "":
            parent = "/".join(d.split("/")[:-1])
            put(0, area, _dir_entry(b".          ", 0x10, dir_clus[d], 0))
            put(32, area, _dir_entry(b"..         ", 0x10,
                                     0 if parent == "" and d.count("/") == 0
                                     else dir_clus[parent], 0))
            off = 64
        else:
            off = pos[""]
        for kind, name, payload in dirs[d]:
            if kind == "dir":
                put(off, area, _dir_entry(_shortname(name), 0x10,
                                          dir_clus[payload], 0))
            else:
                full = d + "/" + name if d else name
                first, data = file_runs[full]
                put(off, area, _dir_entry(_shortname(name), 0x20,
                                          first, len(data)))
            off += 32
        if d == "":
            img[data_start * SECTOR - root_sectors * SECTOR:
                data_start * SECTOR] = area
        else:
            o = clus_off(dir_clus[d])
            img[o:o + SECTOR] = area

    for _, (first, payload) in file_runs.items():
        n = max(1, (len(payload) + SECTOR - 1) // SECTOR)
        o = clus_off(first)
        img[o:o + len(payload)] = payload

    return bytes(img)


if __name__ == "__main__":
    # mkesp.py out.img sectors path1=hostfile1 [path2=hostfile2 ...]
    out_p, sectors = sys.argv[1], int(sys.argv[2])
    files = []
    for spec in sys.argv[3:]:
        name, host = spec.split("=", 1)
        files.append((name, open(host, "rb").read()))
    open(out_p, "wb").write(build_esp(sectors, files))
    print(f"esp: {out_p}, {sectors} sectors, "
          f"{sum(len(d) for _, d in files)} bytes in {len(files)} files")
