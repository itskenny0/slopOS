#!/usr/bin/env python3
"""wifitest.py -- boot PicoOS with a fake esp module on the serial port.

    python3 tools/wifitest.py "wifi" "wifi join myssid mypass" \
        "wifi get http://pico.local/tetris.pico web.pico" "ls"

The serial port goes to tools/espsim.py, which speaks the AT dialect and
does the real http request on this machine, so what comes back into the
guest is a real file.
"""
import sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# COM1 belongs to the esp simulator here, so the screen is read out of
# vga memory through the monitor instead of off the serial line
os.environ["PICO_SERIAL"] = "chardev:esp"
from screen import VM

def main():
    cmds = sys.argv[1:] or ["wifi"]

    extra = ["-chardev", "socket,id=esp,host=127.0.0.1,port=7777,server=off"]
    vm = VM("picoos.img", mem=16, extra=extra, sock="/tmp/wifi.sock")
    time.sleep(2.5)

    for c in cmds:
        wait = 10.0
        if c.startswith("w") and ":" in c:
            wait = float(c[1:c.index(":")])
            c = c[c.index(":") + 1:]
        vm.typ(c)
        time.sleep(wait)
        print("=== %s ===" % c)
        print(vm.screen())
        print()
    vm.kill()

if __name__ == "__main__":
    main()
