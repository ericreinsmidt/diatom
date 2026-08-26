"""Headless conformance: does Diatom still do the same thing, in the same space?

Three properties, and each exists because §11 - the section this project calls
its own thesis - asserted nothing at all:

  determinism   the same core, ROM and frame count produce the same final
                frame. Without this the other checks cannot be trusted, since a
                run that varies has nothing stable to compare.
  sensitivity   N and N+1 frames must DIFFER. The control: a determinism check
                passes perfectly on a frontend that renders one frozen frame,
                so something has to prove the test can fail.
  space         peak RSS stays under a ceiling, and - where the toolchain can
                wrap malloc - Diatom's own allocation count is identical at N
                and 2N frames, which is what "no malloc in the frame loop"
                actually means.

The allocation check needs `-Wl,--wrap`, a GNU ld feature. Apple's ld64 has no
equivalent, so on macOS it reports as skipped rather than passing silently. The
device half, which is the one the memory thesis is about, is
`tools/conform-device.sh`.

    make && make stub && python3 test/conform.py
"""
import hashlib, os, subprocess, sys, time

ROOT   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN    = f"{ROOT}/build/desktop/diatom"
WRAPD  = f"{ROOT}/build/desktop/diatom-conform"
CORE   = f"{ROOT}/build/desktop/stubcore.so"
FRAMES = 300
RSS_CEILING_MB = 32      # measured 12.5 MB here, where SDL and the host window
                         # system dominate. A ceiling well above the measured
                         # figure still fails on the regressions worth catching
                         # and does not fail on a different SDL. The number that
                         # matters is the device's - tools/conform-device.sh

ENV = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_RENDER_DRIVER="software",
           SDL_AUDIODRIVER="dummy")

fails = []
def check(name, ok, detail=""):
    print(("  ok    " if ok else "  FAIL  ") + name + (f"   {detail}" if detail else ""))
    if not ok: fails.append(name)
def skip(name, why):
    print(f"  skip  {name}   {why}")

def run(binary, frames, shot=None):
    """Run one session, returning (peak RSS in kB, stderr). RSS is sampled with
    `ps` rather than getrusage: RUSAGE_CHILDREN only ever grows, so it cannot
    attribute a figure to one child among several."""
    cmd = [binary, "--core", CORE, "--frames", str(frames)]
    if shot: cmd += ["--shot", shot]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                         stderr=subprocess.PIPE, env=ENV)
    peak = 0
    while p.poll() is None:
        try:
            out = subprocess.run(["ps", "-o", "rss=", "-p", str(p.pid)],
                                 capture_output=True, text=True, timeout=5).stdout.strip()
            if out: peak = max(peak, int(out))
        except Exception:
            pass
        time.sleep(0.2)
    return peak, p.stderr.read().decode()

def digest(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()[:16]

if not (os.path.exists(BIN) and os.path.exists(CORE)):
    sys.exit(f"conform: build first - need {BIN} and {CORE}")

print(f"conform: {FRAMES} frames, stubcore, headless\n")

# --- determinism -----------------------------------------------------------
shots = []
peak = 0
for i in range(3):
    s = f"/tmp/diatom-conform-{i}.bmp"
    rss, _ = run(BIN, FRAMES, s)
    peak = max(peak, rss)
    shots.append(digest(s))
check("same frame count, same final frame", len(set(shots)) == 1, " ".join(shots))

# --- sensitivity -----------------------------------------------------------
other = f"/tmp/diatom-conform-plus1.bmp"
run(BIN, FRAMES + 1, other)
d = digest(other)
check(f"{FRAMES} and {FRAMES+1} frames differ", d != shots[0], f"{shots[0]} vs {d}")

# --- space -----------------------------------------------------------------
check(f"peak RSS under {RSS_CEILING_MB} MB", peak / 1024.0 < RSS_CEILING_MB,
      f"{peak/1024.0:.1f} MB")

if os.path.exists(WRAPD):
    a = b = None
    for frames, slot in ((FRAMES, "a"), (FRAMES * 2, "b")):
        _, err = run(WRAPD, frames)
        line = [l for l in err.splitlines() if l.startswith("allocwatch:")]
        n = int(line[0].rsplit("total=", 1)[1]) if line else None
        if slot == "a": a = n
        else: b = n
    check("allocations identical at N and 2N frames", a is not None and a == b,
          f"{a} vs {b}")
else:
    skip("allocations identical at N and 2N frames",
         "no wrapped build (needs GNU ld; run `make conform` on Linux)")

print("\n" + ("PASS" if not fails else f"{len(fails)} FAILED: {fails}"))
sys.exit(1 if fails else 0)
