"""ADR-0020's state plane, end to end against the stub core.

Every case here is one that was actually got wrong while building it, which is
why they are cases rather than a smoke test:

  - the map read as "everything unbound" before any game had run, because
    identity had not been saved yet. Only the IDLE path shows it.
  - a refused SETMAP left the table cleared rather than unchanged.
  - labels have to follow a remap, or a remap screen shows the wrong verbs.

The level half cannot be tested here: a desktop has no volume or brightness of
its own, so `LEVELS count=0` is the whole of it. Levels are exercised on
hardware by `protodrive --state`.

    python3 test/stateplane.py
"""
import socket, subprocess, sys, time, os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOCK = "/tmp/diatom-stateplane.sock"
if os.path.exists(SOCK): os.unlink(SOCK)

p = subprocess.Popen([f"{ROOT}/build/desktop/diatom", "--socket", SOCK],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(100):
    if os.path.exists(SOCK): break
    time.sleep(0.05)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(SOCK)
buf = b""
def lines(timeout=3.0):
    global buf
    s.settimeout(timeout)
    end = time.time() + timeout
    while time.time() < end:
        if b"\n" in buf:
            l, buf = buf.split(b"\n", 1)
            yield l.decode().strip(); end = time.time() + 0.25; continue
        try: d = s.recv(65536)
        except socket.timeout: return
        if not d: return
        buf += d
def send(m):
    s.sendall((m + "\n").encode()); time.sleep(0.35)
def drain(t=1.0): return [l for l in lines(t) if l]

fails = []
def check(name, got, want):
    ok = got == want
    print(("  ok   " if ok else "  FAIL ") + f"{name}\n         got  {got}\n         want {want}")
    if not ok: fails.append(name)

print("READY:", (r := drain(2.0)))
check("proto version", [x for x in r if x.startswith("READY")][0].split("\t")[1], "proto=2")

# Before any RUN: identity must already be identity, not "everything unbound".
send("MAP"); check("map is identity while idle", drain(), ["MAP\tmap=identity"])
send("INPUTS"); check("no labels while idle", drain(), ["INPUTS\tcount=0"])

send(f"RUN\tcore={ROOT}/build/desktop/stubcore.so")
run = drain(3.0)
print("RUN ->", run)

send("INPUTS"); got = drain()
check("labels, port 0 only, partial array", got,
      ["INPUTS\tcount=3", "INPUT\tid=a\tlabel=Fire", "INPUT\tid=b\tlabel=Jump",
       "INPUT\tid=start\tlabel=Pause"])

send("MAP"); check("map starts identity", drain(), ["MAP\tmap=identity"])

send("SETMAP\tmap=x:b,y:a"); check("setmap echoes", drain(), ["MAP\tmap=x:b,y:a"])

send("INPUTS"); got = drain()
check("labels follow the remap", got,
      ["INPUTS\tcount=5", "INPUT\tid=a\tlabel=Fire", "INPUT\tid=b\tlabel=Jump",
       "INPUT\tid=x\tlabel=Jump", "INPUT\tid=y\tlabel=Fire",
       "INPUT\tid=start\tlabel=Pause"])

send("SETMAP\tmap=menu:b"); got = drain()
check("menu is refused, map unchanged", got,
      ["ERROR\tcode=bad_map\tmsg=menu:b", "MAP\tmap=x:b,y:a"])

send("SETMAP\tmap=a:b,nonsense:x"); got = drain()
check("bad pair refuses whole message", got,
      ["ERROR\tcode=bad_map\tmsg=a:b,nonsense:x", "MAP\tmap=x:b,y:a"])

send("SETMAP\tmap=identity"); check("identity clears", drain(), ["MAP\tmap=identity"])

send("LEVELS"); check("desktop has no levels", drain(), ["LEVELS\tcount=0"])
send("SETLEVEL\tkind=brightness\tindex=3\tcount=12")
check("setlevel refused with no control", drain(), ["ERROR\tcode=bad_level\tmsg=brightness"])

send("STOP"); print("STOP ->", drain(2.0))
send("QUIT"); time.sleep(0.4)
s.close(); p.terminate(); p.wait(timeout=5)
print("\n" + ("ALL PASS" if not fails else f"{len(fails)} FAILED: {fails}"))
sys.exit(1 if fails else 0)
