"""Is the resampler adding high frequencies that were not in the source?

A resampler is a lowpass. It cannot create energy above what it was given, so
any window that comes out with MORE high-frequency energy than it went in with
is producing something - imaging, or aliasing folded back down. That is a
falsifiable statement about a filter, which is why this is the measurement
rather than "does it sound right".

Feed it the two sides of `--tap-audio`:

    diatom --core ... --rom ... --tap-audio /tmp/contra
    python3 tools/hfprobe.py /tmp/contra.in.raw /tmp/contra.out.raw

Both files are interleaved S16 stereo. `--rate-in` and `--rate-out` default to
48000; FCEUmm reports 48000 into a 48000 device, which is why Contra is the
fixture - the two sides line up and the comparison needs no resampling of its
own to make it.

Reported per side, over 50 ms windows:

  hf        mean fraction of energy above the cutoff
  cv        its coefficient of variation. A real signal's HF content varies
            smoothly; a resampling artefact tracks the phase of the conversion
            and is far spikier, so this separates "the game is bright" from
            "the filter is ringing".
  worse     percentage of windows where OUT exceeds IN. This is the number
            that cannot be explained away: for a correct lowpass it is noise
            around zero.

An instrument. See tools/README.md.
"""
import sys, numpy as np

def load(path, rate):
    x = np.fromfile(path, dtype="<i2").astype(np.float64)
    if x.size % 2: x = x[:-1]
    return x.reshape(-1, 2).mean(axis=1), rate     # mono sum: artefacts are common-mode

def hf_series(x, rate, cutoff, win_ms=50):
    n = int(rate * win_ms / 1000)
    if n < 16 or x.size < n: return np.array([])
    w = x[:x.size // n * n].reshape(-1, n) * np.hanning(n)
    spec = np.abs(np.fft.rfft(w, axis=1)) ** 2
    freq = np.fft.rfftfreq(n, 1.0 / rate)
    tot  = spec.sum(axis=1)
    hi   = spec[:, freq > cutoff].sum(axis=1)
    keep = tot > 0
    return hi[keep] / tot[keep]

def main(argv):
    a = {"--rate-in": 48000, "--rate-out": 48000, "--cutoff": 10000.0}
    pos = []
    i = 0
    while i < len(argv):
        if argv[i] in a: a[argv[i]] = float(argv[i + 1]); i += 2
        else: pos.append(argv[i]); i += 1
    if len(pos) != 2:
        sys.exit("usage: hfprobe.py [--rate-in N] [--rate-out N] [--cutoff HZ] IN.raw OUT.raw")

    xi, ri = load(pos[0], a["--rate-in"])
    xo, ro = load(pos[1], a["--rate-out"])
    si = hf_series(xi, ri, a["--cutoff"])
    so = hf_series(xo, ro, a["--cutoff"])
    if not si.size or not so.size: sys.exit("hfprobe: not enough audio")

    n = min(si.size, so.size)
    si, so = si[:n], so[:n]
    worse = 100.0 * (so > si).mean()

    print(f"windows          {n} of 50 ms   cutoff {a['--cutoff']:.0f} Hz")
    print(f"{'':17}{'hf':>10}{'cv':>10}")
    for name, s in (("IN  (from core)", si), ("OUT (resampled)", so)):
        cv = s.std() / s.mean() if s.mean() else float("nan")
        print(f"{name:17}{s.mean():10.4f}{cv:10.2f}")
    print(f"\nwindows where OUT exceeds IN: {worse:.1f}%")
    print("  a lowpass cannot add high frequencies, so this is imaging or aliasing"
          if worse > 5.0 else "  consistent with a filter that only removes")

main(sys.argv[1:])
