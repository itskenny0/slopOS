#!/usr/bin/env python3
"""Time how long the console takes to print a screenful of scrolling text.

The complaint that started this was "typing help goes really slowly down the
screen" on a 2011 desktop, which no test here had ever caught: the automated
tests read the serial port, and the serial port does not scroll.

So this measures the thing the user actually sees. It runs the guest under
-icount, which makes the guest clock deterministic and independent of how
busy the host is, sends a command that dumps a few hundred lines, and times
how long the guest takes to get back to a prompt. Two builds measured this
way are directly comparable; two wall-clock runs on a loaded host are not.
"""
import os
import re
import socket
import subprocess
import sys
import time

QROOT = "/tmp/qroot"
ENV = dict(os.environ, LD_LIBRARY_PATH=f"{QROOT}/usr/lib/x86_64-linux-gnu")
SHIFTED = {"&": "shift-7", "_": "shift-minus"}
NAMED = {" ": "spc", "\n": "ret", ".": "dot", "-": "minus", "/": "slash"}


def send(sock, text):
    for ch in text:
        key = SHIFTED.get(ch) or NAMED.get(ch) or ch
        sock.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.03)
        try:
            sock.recv(65536)
        except BlockingIOError:
            pass


def run(image, command, shift, serial, repeat=10):
    sock_path = "/tmp/scroll.sock"
    for path in (sock_path, serial):
        if os.path.exists(path):
            os.remove(path)

    proc = subprocess.Popen(
        [f"{QROOT}/usr/bin/qemu-system-i386", "-L", f"{QROOT}/usr/share/qemu",
         "-drive", f"file={image},format=raw,if=floppy", "-m", "8",
         "-icount", f"shift={shift},align=on", "-display", "none",
         "-serial", f"file:{serial}", "-monitor",
         f"unix:{sock_path},server,nowait", "-no-reboot", "-snapshot"],
        env=ENV, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    for _ in range(200):
        if os.path.exists(sock_path):
            break
        time.sleep(0.1)
    sock = socket.socket(socket.AF_UNIX)
    sock.connect(sock_path)
    time.sleep(0.5)
    sock.recv(65536)

    # wait for the shell to come up
    deadline = time.time() + 120
    while time.time() < deadline:
        if os.path.exists(serial) and "Ready." in open(serial, errors="replace").read():
            break
        time.sleep(0.5)

    # One command is only thirty-odd lines, which is not enough scrolling to
    # measure through the noise. Repeating it is, and typing the repeats
    # costs the same in both builds so it cancels out of the comparison.
    before = len(open(serial, errors="replace").read())
    start = time.time()
    elapsed = None

    for _ in range(repeat):
        send(sock, command + "\n")

    while time.time() - start < 600:
        text = open(serial, errors="replace").read()[before:]
        if text.count("pico>") >= repeat:
            elapsed = time.time() - start
            break
        time.sleep(0.1)

    proc.kill()
    return elapsed


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "picoos.img"
    command = sys.argv[2] if len(sys.argv) > 2 else "hex 0 6000"
    shift = sys.argv[3] if len(sys.argv) > 3 else "8"

    print(f"  {image}: '{command}' at -icount shift={shift}")
    t = run(image, command, shift, "/tmp/scroll.txt")
    if t is None:
        print("  never came back")
        sys.exit(1)
    print(f"  {t:.2f} s")


if __name__ == "__main__":
    main()
