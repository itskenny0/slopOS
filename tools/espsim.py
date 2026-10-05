#!/usr/bin/env python3
"""espsim.py -- pretend to be an ESP8266 running the AT firmware.

There is no emulator for a wireless chip, but there is no need for one to
test the half that is ours: connect QEMU's serial port to this program and
it answers `AT+CWJAP`, `AT+CIPSTART`, `AT+CIPSEND` and friends exactly the
way a module does -- including the +IPD chunks the data arrives in -- and
does the actual HTTP request on this machine, so the bytes that come back
are real bytes from a real server.

    python3 tools/espsim.py [port]

QEMU side:
    -chardev socket,id=esp,host=127.0.0.1,port=7777,server=on,wait=off \
    -serial chardev:esp
"""
import socket, sys, threading, urllib.request

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7777
FAKE_IP = "93.184.216.34"          # what we answer CIPDOMAIN with
# a name the simulator knows: the little web server in tools/nettest land
ALIASES = {"pico.local": "127.0.0.1:8000", "pico": "127.0.0.1:8000"}


class Sim:
    def __init__(self, conn):
        self.c = conn
        self.buf = b""
        self.joined = False
        self.want = 0            # bytes of raw upload still to come
        self.up = b""

    def out(self, s):
        sys.stderr.write("  esp >> %r\n" % s)
        sys.stderr.flush()
        self.c.sendall(s.encode())

    def fetch(self, request):
        """do the http request the module was handed, and return the body"""
        try:
            line = request.split(b"\r\n")[0].decode(errors="replace")
            parts = line.split()
            if len(parts) < 2:
                return b""
            path = parts[1]
            host = ""
            for h in request.split(b"\r\n")[1:]:
                if h.lower().startswith(b"host:"):
                    host = h[5:].strip().decode()
            url = "http://" + (ALIASES.get(host, host)) + path
            with urllib.request.urlopen(url, timeout=20) as r:
                return b"HTTP/1.0 200 OK\r\n\r\n" + r.read()
        except Exception as e:
            sys.stderr.write("espsim fetch failed: %r\n" % (e,))
            return b""

    def handle(self, line):
        u = line.upper()
        if u == "AT":
            self.out("\r\nOK\r\n")
        elif u.startswith("ATE"):
            self.out("\r\nOK\r\n")
        elif u.startswith("AT+CIPMUX"):
            self.out("\r\nOK\r\n")
        elif u.startswith("AT+CWMODE"):
            self.out("\r\nOK\r\n")
        elif u.startswith("AT+CWJAP"):
            self.joined = True
            self.out("\r\nWIFI CONNECTED\r\n\r\nWIFI GOT IP\r\n\r\nOK\r\n")
        elif u.startswith("AT+CWQAP"):
            self.joined = False
            self.out("\r\nOK\r\n")
        elif u.startswith("AT+CIPDOMAIN"):
            self.out("\r\n+CIPDOMAIN:%s\r\n\r\nOK\r\n" % FAKE_IP)
        elif u.startswith("AT+CIPSTART"):
            if not self.joined:
                self.out("\r\nERROR\r\n")
            else:
                self.out("\r\nCONNECT\r\n\r\nOK\r\n")
        elif u.startswith("AT+CIPSEND"):
            try:
                self.want = int(line.split("=")[1])
                self.up = b""
                self.out("\r\nOK\r\n> ")
            except Exception:
                self.out("\r\nERROR\r\n")
        elif u.startswith("AT+CIPCLOSE"):
            self.out("\r\nOK\r\n")
        else:
            self.out("\r\nOK\r\n")

    def run(self):
        self.out("\r\nready\r\n")
        while True:
            try:
                b = self.c.recv(1)
            except Exception:
                return
            if not b:
                return
            if self.want:
                self.up += b
                self.want -= 1
                if self.want == 0:
                    body = self.fetch(self.up)
                    self.out("\r\nSEND OK\r\n")
                    for i in range(0, len(body), 1024):
                        chunk = body[i:i + 1024]
                        self.out("\r\n+IPD,%d:" % len(chunk))
                        self.c.sendall(chunk)
                    self.out("\r\nCLOSED\r\n")
                continue
            if b == b"\n":
                line = self.buf.decode(errors="replace").strip()
                self.buf = b""
                if line:
                    sys.stderr.write("  esp << %r\n" % line)
                    sys.stderr.flush()
                    self.handle(line)
            elif b != b"\r":
                self.buf += b


def main():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(1)
    sys.stderr.write("espsim listening on %d\n" % PORT)
    sys.stderr.flush()
    while True:
        conn, _ = srv.accept()
        sys.stderr.write("espsim: qemu connected\n")
        sys.stderr.flush()
        threading.Thread(target=Sim(conn).run, daemon=True).start()


if __name__ == "__main__":
    main()
