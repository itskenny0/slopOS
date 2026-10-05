import socket, subprocess, time, os, sys
QEMU="/tmp/qroot/usr/bin/qemu-system-i386"
env=dict(os.environ, LD_LIBRARY_PATH="/tmp/qroot/usr/lib/x86_64-linux-gnu")
sock="/tmp/mon.sock"
if os.path.exists(sock): os.remove(sock)
img=sys.argv[1] if len(sys.argv)>1 else "picoos.img"
p=subprocess.Popen([QEMU,"-L","/tmp/qroot/usr/share/qemu","-L","/tmp/qroot/usr/share/qemu","-drive",f"file={img},format=raw,if=floppy","-m","8","-display","none",
  "-serial","file:/tmp/ser.txt","-monitor",f"unix:{sock},server,nowait",
  "-no-reboot"],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.STDOUT)
for _ in range(50):
    if os.path.exists(sock): break
    time.sleep(0.1)
time.sleep(1.5)
s=socket.socket(socket.AF_UNIX); s.connect(sock); time.sleep(0.3); s.recv(65536)
def mon(c):
    s.sendall((c+"\n").encode()); time.sleep(0.35)
    try: return s.recv(65536).decode(errors="replace")
    except: return ""
KEYMAP={' ':'spc','\n':'ret','.':'dot','-':'minus','/':'slash','=':'equal',',':'comma'}
# qemu sendkey has no name for the shifted symbols; spell them out
SHIFTED={'&':'shift-7','!':'shift-1','?':'shift-slash','*':'shift-8','_':'shift-minus',
         '+':'shift-equal','(':'shift-9',')':'shift-0',':':'shift-semicolon'}
def typ(t):
    for ch in t:
        k=SHIFTED.get(ch) or KEYMAP.get(ch, ch)
        if ch.isupper(): k='shift-'+ch.lower()
        mon(f"sendkey {k}")
    mon("sendkey ret")
for cmd in sys.argv[2:]:
    if cmd.startswith("key:"):        # raw qemu key name, e.g. key:ctrl-c
        mon("sendkey " + cmd[4:]); time.sleep(0.5); continue
    if cmd.startswith("wait:"):
        time.sleep(float(cmd[5:])); continue
    typ(cmd); time.sleep(0.5)
time.sleep(0.5)
mon("screendump /tmp/shot.ppm")
time.sleep(0.8)
p.kill()
print(open("/tmp/ser.txt",errors="replace").read())
