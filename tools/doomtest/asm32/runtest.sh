#!/bin/bash
# Validate the 386 FixedMul/FixedDiv against 20000 precomputed vectors.
# Needs gcc with -m32 plus a kernel that runs 32-bit binaries.
set -e
cd "$(dirname "$0")"
TREE="$(cd ../../.. && pwd)"
python3 gen.py
F="-m32 -march=i386 -std=gnu99 -ffreestanding -fno-pie -no-pie -nostdlib -nostdinc -Os -I$TREE/apps/doom/src -I$TREE/apps/doom/libc"
gcc $F -c $TREE/apps/doom/pico/pico_mfixed.c -o mfixed32.o
gcc $F -c test32.c -o test32.o
gcc -m32 -no-pie -nostdlib -o asmtest mfixed32.o test32.o
./asmtest && echo "ASMTEST-OK: 20000 vectors + edge cases match"
