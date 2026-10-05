#!/bin/bash
# Build the Doom host test: the exact same Doom sources + pico layer as
# doom.pico, compiled for x86-64 with the same freestanding headers, linked
# against the syscall-only fake kernel instead of PicoOS. Needs: gcc, python3.
set -e
cd "$(dirname "$0")"
TREE="$(cd ../.. && pwd)"
FLAGS="-O2 -std=gnu99 -ffreestanding -fno-pie -no-pie -fno-stack-protector -fno-builtin -nostdlib -nostdinc -I$TREE/apps -I$TREE/apps/doom/src -I$TREE/apps/doom/libc -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DNDEBUG -Wall -Wno-unused-function -Wno-unused-variable"
mkdir -p obj out hostfs
if [ ! -f hostfs/DOOM1.WAD ]; then
  if [ -f "$TREE/doom1.wad" ]; then cp "$TREE/doom1.wad" hostfs/DOOM1.WAD; else echo "need $TREE/doom1.wad"; exit 1; fi
fi
echo "--- stub + entry ---"
gcc $FLAGS -c stub.c -o obj/stub.o
gcc -c start.S -o obj/start.o
echo "--- doom sources ---"
for f in $TREE/apps/doom/src/*.c $TREE/apps/doom/pico/*.c; do
  b=$(basename $f .c)
  gcc $FLAGS -c $f -o obj/$b.o
done
echo "--- link ---"
gcc -nostdlib -no-pie -o doomtest obj/*.o
echo "BUILD-OK: ./doomtest -timedemo demo1"
