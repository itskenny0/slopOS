#!/bin/sh
# buildapp.sh -- turn one C file into a .pico program.
#
#   sh tools/buildapp.sh template/myapp.c
#   sh tools/buildapp.sh template/myapp.c out/somewhere.pico
#
# This is the whole toolchain for a PicoOS program: compile, link flat,
# strip to a raw binary, wrap in a .pico header. It does not touch the
# kernel and does not need the kernel to be built. Drop the result into an
# image with tools/addapp.py.
#
# Needs: gcc with 32-bit support (gcc-multilib), binutils, python3.
set -e

SRC="$1"
[ -n "$SRC" ] || { echo "usage: sh tools/buildapp.sh <file.c> [out.pico]"; exit 1; }
[ -f "$SRC" ] || { echo "buildapp: no such file: $SRC"; exit 1; }

HERE=$(cd "$(dirname "$0")/.." && pwd)
NAME=$(basename "$SRC" .c)
OUT="${2:-$NAME.pico}"
TMP="${TMPDIR:-/tmp}/picoapp.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

CC=${CC:-gcc}
LD=${LD:-ld}

# The same flags the kernel's own programs are built with. -fno-pie is not
# optional: the default -fpie produces GOT-relative relocations that a flat
# loader cannot resolve, and mkpico.py will refuse the result.
CFLAGS="-m32 -march=i386 -mtune=i386 -std=gnu99 -ffreestanding -fno-pie \
        -fno-stack-protector -fno-asynchronous-unwind-tables -fno-builtin \
        -nostdlib -nostdinc -Wall -Wextra -Os -I$HERE/apps"

$CC $CFLAGS -c "$HERE/apps/crt0.c" -o "$TMP/crt0.o"
$CC $CFLAGS -c "$SRC"              -o "$TMP/app.o"

# -q keeps the relocation records in the ELF so mkpico.py can distil the
# list that lets PicoOS load the program at any address it likes.
$LD -m elf_i386 -T "$HERE/apps/app.ld" -nostdlib --build-id=none \
    --no-warn-rwx-segments -q -o "$TMP/app.elf" "$TMP/crt0.o" "$TMP/app.o"

objcopy -O binary "$TMP/app.elf" "$TMP/app.bin"

python3 "$HERE/tools/mkpico.py" "$TMP/app.elf" "$TMP/app.bin" "$OUT" nm readelf

echo
echo "  now put it on a disk image:"
echo "    python3 tools/addapp.py picoos-hybrid.img $OUT"
