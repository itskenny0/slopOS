import socket, subprocess, time, os, sys
QEMU="/tmp/qroot/usr/bin/qemu-system-x86_64"
env=dict(os.environ, LD_LIBRARY_PATH="/tmp/qroot/usr/lib/x86_64-linux-gnu")
sock="/tmp/mon2.sock"
if os.path.exists(sock): os.remove(sock)
img=sys.argv[1]; wait=float(sys.argv[2]) if len(sys.argv)>2 else 20
p=subprocess.Popen([QEMU,"-L","/tmp/qroot/usr/share/qemu","-bios","/tmp/ovmf.fd",
  "-drive",f"file={img},format=raw,if=ide","-m","256","-display","none",
  "-monitor",f"unix:{sock},server,nowait","-no-reboot"],env=env,
  stdout=subprocess.DEVNULL,stderr=subprocess.STDOUT)
for _ in range(80):
    if os.path.exists(sock): break
    time.sleep(0.1)
time.sleep(1)
s=socket.socket(socket.AF_UNIX); s.connect(sock); time.sleep(0.3); s.recv(65536)
def mon(c):
    s.sendall((c+"\n").encode()); time.sleep(0.4)
    try: return s.recv(65536).decode(errors="replace")
    except: return ""
time.sleep(wait)
for cmd in sys.argv[3:]:
    for ch in cmd:
        mon("sendkey "+{' ':'spc','.':'dot','-':'minus','/':'slash'}.get(ch,ch))
    mon("sendkey ret"); time.sleep(0.6)
time.sleep(1)
mon("screendump /tmp/ushot.ppm"); time.sleep(1.5)
p.kill()
print("done")
