#!/bin/sh
# matrix.sh -- boot every image on every firmware we can emulate.
#
# "Runs on almost anything" is a claim, and a claim you do not re-test is a
# claim that quietly stops being true. Nine configurations: three machine
# sizes on the floppy image, the hybrid and GPT images under BIOS and under
# UEFI, and the CD. A configuration passes when the kernel reaches its
# "Ready" line on the serial port -- booting is not enough, it has to get
# all the way to a usable shell.
#
# Always -snapshot: OVMF writes its variable store back into the EFI system
# partition, and without the flag a test run mutates the image it tests.
Q=/tmp/qroot
export LD_LIBRARY_PATH=$Q/usr/lib/x86_64-linux-gnu:$Q/usr/lib
I386=$Q/usr/bin/qemu-system-i386
X64=$Q/usr/bin/qemu-system-x86_64
pass=0; fail=0

check() {
    name="$1"; shift
    rm -f /tmp/mx.ser
    timeout 40 "$@" -display none -serial file:/tmp/mx.ser -no-reboot -snapshot \
        >/dev/null 2>&1 &
    pid=$!
    ok=0
    i=0
    while [ $i -lt 35 ]; do
        if [ -f /tmp/mx.ser ] && grep -q "Ready" /tmp/mx.ser 2>/dev/null; then ok=1; break; fi
        sleep 1; i=$((i+1))
    done
    kill $pid 2>/dev/null; wait $pid 2>/dev/null
    if [ $ok = 1 ]; then echo "  [pass] $name"; pass=$((pass+1))
    else                 echo "  [FAIL] $name"; fail=$((fail+1)); fi
}

echo "PicoOS boot matrix"
check "floppy  BIOS  486     1 MB"  $I386 -L $Q/usr/share/qemu -cpu 486 -m 1 \
      -drive file=picoos.img,format=raw,if=floppy
check "floppy  BIOS  pentium 4 MB"  $I386 -L $Q/usr/share/qemu -cpu pentium -m 4 \
      -drive file=picoos.img,format=raw,if=floppy
check "floppy  BIOS  qemu32  64 MB" $I386 -L $Q/usr/share/qemu -m 64 \
      -drive file=picoos.img,format=raw,if=floppy
check "hybrid  BIOS  ide     8 MB"  $I386 -L $Q/usr/share/qemu -m 8 \
      -drive file=picoos-usb.img,format=raw,if=ide
check "hybrid  BIOS  usb-ish 16 MB" $I386 -L $Q/usr/share/qemu -m 16 \
      -drive file=picoos-usb.img,format=raw,if=ide
check "cdrom   BIOS  El Torito"     $I386 -L $Q/usr/share/qemu -m 16 \
      -cdrom picoos.iso -boot d
check "hybrid  UEFI  OVMF"          $X64 -L $Q/usr/share/qemu -m 256 \
      -bios /tmp/ovmf_mx.fd -drive file=picoos-usb.img,format=raw,if=ide
check "gpt     UEFI  OVMF"          $X64 -L $Q/usr/share/qemu -m 256 \
      -bios /tmp/ovmf_mx.fd -drive file=extra/picoos-uefi-only.img,format=raw,if=ide
check "gpt     UEFI  512 MB"        $X64 -L $Q/usr/share/qemu -m 512 \
      -bios /tmp/ovmf_mx.fd -drive file=extra/picoos-uefi-only.img,format=raw,if=ide

echo "  ---- $pass passed, $fail failed"
[ $fail = 0 ]
