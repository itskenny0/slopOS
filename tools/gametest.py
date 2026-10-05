#!/usr/bin/env python3
"""Drive a .pico game inside QEMU and photograph the result.

Everything runs with -snapshot. UEFI firmware writes its variable store
back to the EFI system partition, so without that flag a test run would
mutate the very image it is testing and the next run would not be
independent -- which is exactly the trap that cost an afternoon once.

Games do not print to the serial port, so the only way to check that tetris
actually works is to play it: boot the image, type the command, send real
key presses through the QEMU monitor and take screenshots along the way.

  python3 tools/gametest.py picoos.img "start tetris" \
          "wait:2,left,left,up,spc,wait:1,shot,type:ls" shots/tetris

Script steps: a key name, key*N to repeat, wait:SECONDS, shot, or
type:TEXT to type a shell command and press enter.

Add --uefi to run the hybrid image under OVMF instead.
"""
import os
import socket
import struct
import subprocess
import sys
import time
import zlib

QROOT = "/tmp/qroot"
ENV = dict(os.environ,
           LD_LIBRARY_PATH=f"{QROOT}/usr/lib/x86_64-linux-gnu:{QROOT}/usr/lib")
SOCK = "/tmp/game.sock"

KEYMAP = {
    " ": "spc", "\n": "ret", ".": "dot", "-": "minus", "/": "slash",
    "=": "equal", ",": "comma", "_": "shift-minus",
}


def ppm_to_png(src, dst):
    data = open(src, "rb").read()
    if not data.startswith(b"P6"):
        raise SystemExit(f"{src}: not a binary ppm")
    vals, pos = [], 2
    while len(vals) < 3:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        tok = b""
        while not data[pos:pos + 1].isspace():
            tok += data[pos:pos + 1]
            pos += 1
        vals.append(int(tok))
    pos += 1
    w, h, _ = vals
    raw = data[pos:pos + w * h * 3]
    rows = b"".join(b"\x00" + raw[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(rows, 9)) +
           chunk(b"IEND", b""))
    open(dst, "wb").write(png)
    return w, h


class Machine:
    def __init__(self, image, uefi=False):
        if os.path.exists(SOCK):
            os.remove(SOCK)
        if uefi:
            fw = "/tmp/ovmf_game.fd"
            subprocess.run(["cp", f"{QROOT}/usr/share/ovmf/OVMF.fd", fw], check=True)
            binary = f"{QROOT}/usr/bin/qemu-system-x86_64"
            drive = ["-bios", fw, "-snapshot",
                     "-drive", f"file={image},format=raw,if=ide", "-m", "256"]
        else:
            binary = f"{QROOT}/usr/bin/qemu-system-i386"
            mem = os.environ.get("PICO_MEM", "8")
            drive = ["-snapshot",
                     "-drive", f"file={image},format=raw,if=floppy", "-m", mem]
            cpu = os.environ.get("PICO_CPU")
            if cpu:
                drive += ["-cpu", cpu]
            # PICO_ICOUNT=10 throttles the CPU to roughly a 4 MHz 386,
            # with align=on tying virtual time to the wall clock
            icount = os.environ.get("PICO_ICOUNT")
            if icount:
                drive += ["-icount", f"shift={icount},align=on"]

        self.proc = subprocess.Popen(
            [binary, "-L", f"{QROOT}/usr/share/qemu"] + drive +
            ["-display", "none", "-serial", "file:/tmp/game.ser",
             "-monitor", f"unix:{SOCK},server,nowait", "-no-reboot"],
            env=ENV, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

        for _ in range(100):
            if os.path.exists(SOCK):
                break
            time.sleep(0.1)
        time.sleep(1.0)
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(SOCK)
        time.sleep(0.3)
        self.sock.recv(1 << 16)

    def mon(self, cmd, pause=0.2):
        # Read until the monitor gives its prompt back, rather than sleeping
        # a guessed number of milliseconds and hoping. A slow reply (a
        # screendump of a 720x400 screen is the usual one) otherwise stays in
        # the socket and lands in front of the *next* command's reply, and
        # from that point on every keystroke in the script is swallowed. The
        # symptom is a test that stops responding halfway through and looks
        # exactly like a frozen operating system.
        self.sock.sendall((cmd + "\n").encode())
        out = ""
        deadline = time.time() + 5.0
        self.sock.settimeout(0.2)
        while time.time() < deadline:
            try:
                chunk = self.sock.recv(1 << 16).decode(errors="replace")
            except OSError:
                chunk = ""
            out += chunk
            if out.rstrip().endswith("(qemu)"):
                break
        self.sock.settimeout(None)
        time.sleep(pause)
        return out

    def key(self, name, pause=0.12):
        self.mon("sendkey " + name, pause)

    def type(self, text):
        for ch in text:
            self.key(KEYMAP.get(ch, ch), 0.08)
        self.key("ret", 0.3)

    def shot(self, path):
        # Drain the monitor afterwards. screendump's reply arrives in its own
        # good time, and whatever is left unread gets prepended to the reply
        # of the next command -- after which every sendkey in the rest of the
        # script quietly goes nowhere. That cost an hour of hunting a
        # keyboard bug in the OS that was never there.
        self.mon("screendump /tmp/game.ppm", 0.4)
        w, h = ppm_to_png("/tmp/game.ppm", path)
        print(f"  saved {path} ({w}x{h})")

    def kill(self):
        try:
            self.proc.kill()
        except Exception:
            pass


def main():
    args = [a for a in sys.argv[1:] if a != "--uefi"]
    uefi = "--uefi" in sys.argv
    if len(args) < 4:
        sys.exit(__doc__)
    image, command, script, prefix = args[0], args[1], args[2], args[3]

    os.makedirs(os.path.dirname(prefix) or ".", exist_ok=True)

    m = Machine(image, uefi)
    boot_wait = 14 if uefi else 6
    print(f"  booting ({boot_wait}s)...")
    time.sleep(boot_wait)

    m.type(command)
    time.sleep(1.5)

    shot_no = 0
    for step in script.split(","):
        step = step.strip()
        if not step:
            continue
        if step.startswith("wait:"):
            time.sleep(float(step[5:]))
        elif step.startswith("type:"):
            m.type(step[5:])
            time.sleep(1.0)
        elif step == "shot":
            m.shot(f"{prefix}-{shot_no}.png")
            shot_no += 1
        else:
            n = 1
            if "*" in step:
                step, n = step.split("*")
                n = int(n)
            for _ in range(n):
                m.key(step)

    m.shot(f"{prefix}-{shot_no}.png")
    m.kill()


if __name__ == "__main__":
    main()
