"""Does the state plane answer on the wire?

ADR-0020 defines a state as three things - a query verb, a write verb, and the
query verb repeated unsolicited when Diatom changes it. Every part of that is a
promise to a launcher in another repository, and none of it is visible to a
compiler: a verb can be added to the enum, parsed, dispatched, and still never
reach a socket because one branch returns early.

So this speaks the protocol. It starts Diatom with no core and no ROM - the
resident, idle state a launcher connects to first - and asks.

Audio output (ADR-0029) is what it covers today, because that state's whole
point is a device that can FAIL, and the failure has to arrive as an answer
rather than as an error or a silence. The others predate this file; add them
here when one of them next needs changing.

Fast and offline: no core, no ROM, no frames, about a second. It skips rather
than builds when there is no binary, so `make check` keeps costing nothing.
"""
import os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN  = f"{ROOT}/build/desktop/diatom"
# Short, because a Unix socket path is capped near 104 bytes and a scratch
# directory blows straight through it. The failure is "socket path too long",
# which reads like a bug in the thing under test.
SOCK = "/tmp/diatom-proto-check.sock"
ENV  = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_RENDER_DRIVER="software",
            SDL_AUDIODRIVER="dummy")

fails = []
def ck(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok: fails.append(what)


class Diatom:
    def __enter__(self):
        if os.path.exists(SOCK): os.unlink(SOCK)
        self.p = subprocess.Popen([BIN, "--socket", SOCK], env=ENV,
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists(SOCK): break
            time.sleep(0.05)
        else:
            raise RuntimeError("diatom never listened")
        self.c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.c.connect(SOCK)
        self.c.settimeout(2.0)
        self.buf = b""
        return self

    def __exit__(self, *a):
        self.c.close()
        self.p.terminate()
        self.p.wait()
        if os.path.exists(SOCK): os.unlink(SOCK)

    def send(self, line):
        self.c.sendall(line.encode() + b"\n")

    def line(self, prefix=None):
        """The next line, or the next one starting with `prefix`.

        Filtering matters: these are a shared channel and an unsolicited event
        may land between a question and its answer, which is the plane working
        rather than a fault."""
        deadline = time.time() + 2.0
        while time.time() < deadline:
            while b"\n" in self.buf:
                l, self.buf = self.buf.split(b"\n", 1)
                l = l.decode()
                if prefix is None or l.startswith(prefix): return l
            try:
                d = self.c.recv(4096)
            except socket.timeout:
                break
            if not d: break
            self.buf += d
        return None


def main():
    if not os.path.exists(BIN):
        print(f"  skip  protocol: no {os.path.relpath(BIN, ROOT)}; run make first")
        return 0

    with Diatom() as d:
        ready = d.line("READY")
        print(f"  READY: {ready}")
        ck(ready is not None and "proto=6" in ready,
           "READY announces proto=6, so a launcher knows quiet exists")

        # Query. Empty is a real value - the port's default device - and the
        # launcher has to be able to tell it from "no answer".
        d.send("AUDIO")
        r = d.line("AUDIO")
        ck(r == "AUDIO\tdevice=", "AUDIO answers, and the default reads as empty")

        # The write, refused by the hardware. ADR-0029: it falls back and
        # reports where the sound IS, and it is never an error, because a
        # launcher treating it as one is what killed the resident emulator.
        d.send("SETAUDIO\tdevice=no such sink anywhere")
        r = d.line()
        ck(r == "AUDIO\tdevice=",
           "a sink that will not open answers with where it actually landed")
        ck(r is not None and not r.startswith("ERROR"),
           "and never with an ERROR")

        d.send("SETAUDIO\tdevice=")
        ck(d.line("AUDIO") == "AUDIO\tdevice=", "an empty device means the default")

        # Idempotent. A write on the state plane re-states as often as a
        # launcher likes, and re-stating the CURRENT device must not close and
        # reopen a working sink - that drops whatever is queued and costs a
        # fresh stream setup. Measured 2026-09-05: a launcher flapping between
        # two routes reopened the device nine times in one game and dropped
        # 407236 audio frames. (That the queue survives is a port-level
        # property this cannot see; here it only has to keep answering.)
        d.send("SETAUDIO\tdevice=")
        r = d.line()
        ck(r == "AUDIO\tdevice=", "re-stating the same device answers the same")
        ck(r is not None and not r.startswith("ERROR"), "and is not an error")

        # Quiet, ADR-0032: the launcher's to set and nobody else's, so it
        # starts off, says what it was set to, and a value that is not "1"
        # lets the game be heard rather than holding it silent.
        d.send("QUIET")
        ck(d.line("QUIET") == "QUIET\ton=0", "QUIET answers, off before anyone sets it")
        d.send("SETQUIET\ton=1")
        ck(d.line("QUIET") == "QUIET\ton=1", "SETQUIET answers with the new state")
        d.send("QUIET")
        ck(d.line("QUIET") == "QUIET\ton=1", "and still holds it on the next ask")
        d.send("SETQUIET\ton=true")
        ck(d.line("QUIET") == "QUIET\ton=0", "a value that is not 1 releases it")

        # ADR-0009's forward-compatibility promise, which is what lets a verb
        # be added at all. If this ever fails, adding one stops being safe.
        d.send("NOSUCHVERB\tdevice=x")
        d.send("AUDIO")
        ck(d.line("AUDIO") is not None, "an unknown verb is ignored, not fatal")

    # ADR-0033: the newest connection wins. On 2026-09-18 a daemon the
    # launcher had started inherited the launcher's end of this socket and kept
    # it open after the launcher died. The connection never reached EOF, this
    # process never looked at the listener while it had one, and the restarted
    # launcher blocked in connect before its first frame - the Brick froze.
    #
    # So: hold a connection open and never read it again, the way that daemon
    # did, then connect three more times, each while the one before is held.
    # Every one has to be greeted - READY is what an accepted launcher hears
    # first - which the old code never did for any of them.
    def ready(c):
        buf, deadline = b"", time.time() + 2.0
        while time.time() < deadline:
            try:
                got = c.recv(4096)
            except OSError:
                return False
            if not got: return False
            buf += got
            if b"READY" in buf: return True
        return False

    with Diatom() as d:
        d.line("READY")
        stale, newer, greeted = d.c, [], True
        for _ in range(3):
            c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            c.settimeout(2.0)
            try:
                c.connect(SOCK)
            except OSError:
                c.close()
                greeted = False
                break
            newer.append(c)
            if not ready(c):
                greeted = False
                break
        ck(greeted, "a connect while one is held is accepted, three times over")
        if newer:
            last, buf, ans = newer[-1], b"", None
            last.sendall(b"AUDIO\n")
            deadline = time.time() + 2.0
            while ans is None and time.time() < deadline:
                try:
                    got = last.recv(4096)
                except socket.timeout:
                    break
                if not got: break
                buf += got
                ans = next((l for l in buf.decode().split("\n")
                            if l.startswith("AUDIO")), None)
            ck(ans == "AUDIO\tdevice=", "the newest connection is the one answered")

            def closed(c):
                c.settimeout(2.0)
                try:
                    while True:
                        got = c.recv(4096)
                        if not got: return True
                except OSError:
                    return False
            ck(closed(stale), "the connection that was held open is closed")
            ck(all(closed(c) for c in newer[:-1]),
               "and so is every one the newest displaced")
        for c in newer: c.close()

    if fails:
        print(f"\n{len(fails)} protocol check(s) failed")
        return 1
    print("\nok: the state plane answers on the wire")
    return 0


if __name__ == "__main__":
    sys.exit(main())
