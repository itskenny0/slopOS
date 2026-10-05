#!/usr/bin/env python3
"""nettest.py -- boot PicoOS with a network card and try the internet.

    python3 tools/nettest.py rtl8139 "net" "dhcp" "ping 10.0.2.2" ...

The card is a command line argument so the same script exercises every
driver: rtl8139, e1000, pcnet, ne2000.
"""
import sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from screen import VM

QEMU = os.environ.get("QEMU", "/tmp/qroot/usr/bin/qemu-system-i386")

CARDS = {
    "rtl8139": ["-device", "rtl8139,netdev=n0"],
    "e1000":   ["-device", "e1000,netdev=n0"],
    "pcnet":   ["-device", "pcnet,netdev=n0"],
    "ne2000":  ["-device", "ne2k_isa,iobase=0x300,irq=9,netdev=n0"],
    "virtio":  ["-device", "virtio-net-pci,netdev=n0"],
}

def main():
    card = sys.argv[1] if len(sys.argv) > 1 else "rtl8139"
    cmds = sys.argv[2:] or ["net", "dhcp", "ping 10.0.2.2"]

    extra = CARDS[card] + ["-netdev", "user,id=n0"]
    vm = VM("picoos.img", mem=16, extra=extra, sock="/tmp/net%s.sock" % card)
    time.sleep(2.5)

    print("=== boot ===")
    print(vm.screen())

    for c in cmds:
        if c.startswith("wait:"):
            time.sleep(float(c[5:]))
            continue
        print("=== %s ===" % c)
        vm.typ(c)
        time.sleep(4.0 if c.startswith("wget") or c == "dhcp" else 2.5)
        print(vm.screen())
        print()

    vm.kill()

if __name__ == "__main__":
    main()
