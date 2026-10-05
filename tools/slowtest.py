#!/usr/bin/env python3
"""Boot PicoOS on a deliberately crippled CPU and time it.

QEMU's -icount gives every instruction a fixed cost of 2^shift nanoseconds.
On its own that only rescales the *virtual* clock -- the emulator still runs
flat out and boots in a fifth of a second. The option that matters is
align=on, which ties virtual time to the wall clock, so the guest genuinely
executes only 1e9 / 2^shift instructions per real second, timer interrupts
and all, and a stopwatch on the host measures simulated seconds directly.

shift is capped at 10, which is about a 4 MHz 386. Below that the numbers
have to be extrapolated -- boot time scales linearly with the shift, so
each extra halving of the clock doubles it.

A 386 averages roughly 4.5 clock cycles per instruction, so:

    instructions/sec = 1e9 / 2^shift
    equivalent MHz   = instructions/sec * 4.5 / 1e6

  python3 tools/slowtest.py 6 8 9 10
"""
import os
import subprocess
import sys
import time

QROOT = "/tmp/qroot"
ENV = dict(os.environ,
           LD_LIBRARY_PATH=f"{QROOT}/usr/lib/x86_64-linux-gnu:{QROOT}/usr/lib")
CPI = 4.5          # clocks per instruction on a 386


def equiv_mhz(shift):
    ips = 1e9 / (2 ** shift)
    return ips * CPI / 1e6, ips


def run(shift, image="picoos.img", limit=240):
    ser = f"/tmp/slow{shift}.txt"
    if os.path.exists(ser):
        os.remove(ser)

    proc = subprocess.Popen(
        [f"{QROOT}/usr/bin/qemu-system-i386", "-L", f"{QROOT}/usr/share/qemu",
         "-snapshot", "-cpu", "486", "-m", "4",
         "-icount", f"shift={shift},align=on",
         "-drive", f"file={image},format=raw,if=floppy",
         "-display", "none", "-serial", f"file:{ser}", "-no-reboot"],
        env=ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    start = time.time()
    first_byte = None
    ready = None

    while time.time() - start < limit:
        if proc.poll() is not None:
            break
        try:
            data = open(ser, "rb").read()
        except OSError:
            data = b""
        if data and first_byte is None:
            first_byte = time.time() - start
        if b"Ready" in data:
            ready = time.time() - start
            break
        time.sleep(0.1)

    proc.kill()
    proc.wait()
    return first_byte, ready


def main():
    shifts = [int(a) for a in sys.argv[1:]] or [6, 8, 9, 10]
    for s in shifts:
        if s > 10:
            sys.exit("qemu only accepts shift values up to 10")
    print(f"  {'shift':>5}  {'instr/sec':>10}  {'~386 MHz':>9}  "
          f"{'BIOS+load':>10}  {'to shell':>9}")
    print("  " + "-" * 52)
    for s in shifts:
        mhz, ips = equiv_mhz(s)
        fb, rdy = run(s)
        fb_s = f"{fb:8.1f} s" if fb else "     n/a"
        rdy_s = f"{rdy:7.1f} s" if rdy else "  FAILED"
        print(f"  {s:5d}  {ips/1e6:8.2f} M  {mhz:8.1f}  {fb_s:>10}  {rdy_s:>9}")


if __name__ == "__main__":
    main()
