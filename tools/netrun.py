#!/usr/bin/env python3
"""netrun.py -- boot with a card, run a list of commands, print the serial log.

    python3 tools/netrun.py rtl8139 "dhcp" "ping 10.0.2.2" "wget http://..."

Every command gets `wait` seconds (default 8); a command may start with
`w<seconds>:` to ask for a different one, e.g. "w20:wget http://x/y".
"""
import sys, time, os
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
    cmds = sys.argv[2:] or ["net", "dhcp", "ping 10.0.2.2"]

    if os.path.exists(SER):
        os.remove(SER)
    extra = CARDS[card] + ["-netdev", "user,id=n0"]
    vm = VM("picoos.img", mem=16, extra=extra, sock="/tmp/run.sock")
    time.sleep(2.5)

    for c in cmds:
        wait = 8.0
        if c.startswith("w") and ":" in c:
            wait = float(c[1:c.index(":")])
            c = c[c.index(":") + 1:]
        before = len(ser())
        vm.typ(c)
        time.sleep(wait)
        print("=== %s ===" % c)
        print(ser()[before:].replace("\r", "").rstrip())
        print()
    vm.kill()

if __name__ == "__main__":
    main()
