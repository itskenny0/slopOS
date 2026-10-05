#!/usr/bin/env python3
"""netdbg.py -- boot, send a command, and read the serial log (which has
every character the kernel printed, scrolled or not).

    python3 tools/netdbg.py rtl8139 "dhcp" 20
"""
import sys, time, os, subprocess, socket
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from screen import VM

CARDS = {
    "rtl8139": ["-device", "rtl8139,netdev=n0"],
    "e1000":   ["-device", "e1000,netdev=n0"],
    "pcnet":   ["-device", "pcnet,netdev=n0"],
    "ne2000":  ["-device", "ne2k_isa,iobase=0x300,irq=9,netdev=n0"],
}
SER = "/tmp/screen.ser"

def ser():
    try:
        return open(SER, errors="replace").read()
    except Exception:
        return ""

def main():
    card = sys.argv[1] if len(sys.argv) > 1 else "rtl8139"
    cmd  = sys.argv[2] if len(sys.argv) > 2 else "net"
    wait = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0

    if os.path.exists(SER):
        os.remove(SER)
    extra = CARDS[card] + ["-netdev", "user,id=n0"]
    vm = VM("picoos.img", mem=16, extra=extra, sock="/tmp/dbg.sock")
    time.sleep(2.5)
    before = len(ser())
    vm.typ(cmd)
    time.sleep(wait)
    out = ser()[before:]
    print("--- serial output for %r ---" % cmd)
    print(out.replace("\r", ""))
    print("--- end ---")
    vm.kill()

if __name__ == "__main__":
    main()
