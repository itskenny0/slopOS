#!/usr/bin/env python3
"""screen.py -- drive PicoOS in QEMU and read the screen back as text.

Boots an image, types things at it, and prints the 80x25 text screen by
reading VGA memory straight out of the guest with the monitor's `xp`. That
is the only way to check what a text-mode desktop actually drew without
eyeballing a screenshot every single time.

    python3 tools/screen.py picoos.img "menu" "wait:2" "key:f10" ...

Commands are either text (typed, then Return) or one of:
    key:<qemu key name>   sendkey, e.g. key:f10, key:down, key:tab
    wait:<seconds>        pause
    shot:<path>           screendump to a .ppm
"""
import socket, subprocess, time, os, sys, re

QEMU = os.environ.get("QEMU", "/tmp/qroot/usr/bin/qemu-system-i386")
BIOS = os.environ.get("OVMF", "/tmp/ovmf_mx.fd")
env = dict(os.environ, LD_LIBRARY_PATH="/tmp/qroot/usr/lib/x86_64-linux-gnu")

# The kernel scrolls the text console by moving the CRTC start address, so
# a screen dump has to begin at `origin`, not at 0xB8000. The address comes
# out of the kernel's symbol table.
ORIGIN_ADDR = 0
try:
    import subprocess
    _nm = subprocess.run(["nm", "build/kernel.elf"], capture_output=True, text=True)
    for _line in _nm.stdout.splitlines():
        if _line.endswith(" b origin"):
            ORIGIN_ADDR = int(_line.split()[0], 16)
            break
except Exception:
    pass

KEYMAP = {' ': 'spc', '.': 'dot', '-': 'minus', '/': 'slash', '=': 'equal',
          ',': 'comma', ';': 'semicolon', "'": 'apostrophe'}
SHIFTED = {'&': 'shift-7', '!': 'shift-1', '?': 'shift-slash', '*': 'shift-8',
           '_': 'shift-minus', '+': 'shift-equal', '(': 'shift-9',
           ')': 'shift-0', ':': 'shift-semicolon', '"': 'shift-apostrophe'}


class VM:
    def __init__(self, img, mem=8, extra=(), sock="/tmp/screen.sock", uefi=False):
        if os.path.exists(sock):
            os.remove(sock)
        self.sock = sock
        cmd = [QEMU, "-L", "/tmp/qroot/usr/share/qemu",
               "-drive", "file=%s,format=raw,if=%s" % (img, "ide" if uefi else "floppy"),
               "-m", str(mem), "-display", "none",
               "-monitor", "unix:%s,server,nowait" % sock,
               "-serial", os.environ.get("PICO_SERIAL", "file:/tmp/screen.ser"),
               "-no-reboot", "-snapshot"]
        cmd += list(extra)
        if uefi:
            cmd += ["-bios", BIOS]
        self.err = open("/tmp/qemu_err.txt", "wb")
        self.p = subprocess.Popen(cmd, env=env,
                                  stdout=subprocess.DEVNULL, stderr=self.err)
        for _ in range(60):
            if os.path.exists(sock):
                break
            time.sleep(0.1)
        if not os.path.exists(sock):
            self.err.flush()
            raise RuntimeError("qemu did not start:\n" +
                               open("/tmp/qemu_err.txt", errors="replace").read())
        time.sleep(0.4)
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(sock)
        self.s.settimeout(0.5)
        try:
            self.s.recv(65536)
        except Exception:
            pass

    def mon(self, c, wait=0.25):
        try:
            self.s.sendall((c + "\n").encode())
        except Exception:
            return ""
        time.sleep(wait)
        out = b""
        while True:
            try:
                b = self.s.recv(65536)
            except Exception:
                break
            if not b:
                break
            out += b
            if len(out) > 4 << 20:
                break
        return out.decode(errors="replace")

    def key(self, k):
        self.mon("sendkey " + k, 0.18)

    def typ(self, t):
        for ch in t:
            k = SHIFTED.get(ch) or KEYMAP.get(ch, ch)
            if ch.isupper():
                k = "shift-" + ch.lower()
            self.key(k)
        self.key("ret")

    def dump(self, rows=25):
        """the visible 80x25 text screen, as a list of strings.

        The console scrolls by moving the CRTC start address, so what you
        see is not necessarily vram row 0 -- read the kernel's origin out
        of guest memory and start there instead.
        """
        org = self.origin()
        return self.mem(0xB8000 + org * 2, rows * 160)

    def origin(self):
        addr = ORIGIN_ADDR
        if not addr:
            return 0
        txt = self.mon("xp /1xw 0x%x" % addr, 0.4)
        hits = re.findall(r":\s*0x([0-9a-fA-F]+)", txt)
        return int(hits[-1], 16) if hits else 0

    def cells(self, rows=25):
        """every visible cell as (character, attribute) pairs.

        A highlight that moves is a colour change, not a text change, so a
        test that only compares characters cannot see the desktop respond.
        """
        vals = self.mem_bytes(0xB8000 + self.origin() * 2, rows * 160)
        return [(int(vals[i * 2], 16), int(vals[i * 2 + 1], 16))
                for i in range(rows * 80)]

    def mem(self, addr, n):
        vals = self.mem_bytes(addr, n)
        out, row = [], []
        for i, v in enumerate(vals):
            if i % 2:
                continue
            c = int(v, 16)
            row.append(chr(c) if 32 <= c < 127 else " ")
            if len(row) == 80:
                out.append("".join(row).rstrip())
                row = []
        return out

    def mem_bytes(self, addr, n):
        txt = self.mon("xp /%db 0x%x" % (n, addr), 0.5)
        vals = []
        for line in txt.splitlines():
            # the monitor prints "0x00000000000b8000: 0x20 0x07 ..."; the
            # address itself also looks like a byte, so only take what
            # follows the colon
            body = line.split(":", 1)[1] if ":" in line else ""
            vals += re.findall(r"0x([0-9a-fA-F]{2})", body)
        return vals

    def screen(self):
        return "\n".join(self.dump())

    def kill(self):
        try:
            self.p.kill()
        except Exception:
            pass


def main():
    args = sys.argv[1:]
    img = args[0] if args else "picoos.img"
    vm = VM(img)
    time.sleep(2.0)
    for c in args[1:]:
        if c.startswith("key:"):
            vm.key(c[4:])
        elif c.startswith("wait:"):
            time.sleep(float(c[5:]))
        elif c.startswith("shot:"):
            vm.mon("screendump " + c[5:], 0.8)
        elif c.startswith("raw:"):
            vm.mon(c[4:], 0.4)
        else:
            vm.typ(c)
            time.sleep(0.5)
    print(vm.screen())
    if os.path.exists("/tmp/screen.ser"):
        sys.stderr.write(open("/tmp/screen.ser", errors="replace").read())
    vm.kill()


if __name__ == "__main__":
    main()
