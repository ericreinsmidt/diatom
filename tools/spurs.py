"""How much of the output is not the tone that went in?

`resampleprobe` pushes a single sine through the real resampler. The input has
exactly one spectral component, so everything else in the output was made by the
filter. That makes the number unarguable in a way a game recording is not - see
the header of tools/resampleprobe.c for how measuring Contra reached the wrong
conclusion twice.

    python3 tools/spurs.py <out.raw> <dst_rate> <tone_hz>

SFDR is the loudest spur relative to the tone, in dB below it. Bigger is better.
Linear interpolation should look poor here; a windowed sinc should not.

An instrument. See tools/README.md.
"""
import sys, numpy as np

path, rate, tone = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
x = np.fromfile(path, dtype="<i2").astype(float)
x = x[: x.size // 2 * 2].reshape(-1, 2)[:, 0]

# Skip the start-up transient. The resampler is primed with a window of silence
# and its edge is a step, which any decent filter legitimately rings on; that
# ringing is not a steady-state property and should not be scored as one.
x = x[int(rate * 0.25):]
n = 1 << 16
if x.size < n:
    sys.exit("spurs: need more audio")
x = x[:n]

spec = np.abs(np.fft.rfft(x * np.hanning(n))) ** 2
freq = np.fft.rfftfreq(n, 1.0 / rate)

k = int(np.argmin(np.abs(freq - tone)))
band = 12                          # a Hanning window smears a tone over a few bins
sig = spec[max(0, k - band):k + band + 1].sum()

rest = spec.copy()
rest[max(0, k - band):k + band + 1] = 0
rest[:8] = 0                       # DC, and the window's own skirt around it
j = int(np.argmax(rest))

sfdr = 10 * np.log10(sig / rest[j]) if rest[j] > 0 else float("inf")
thdn = 10 * np.log10(sig / rest.sum()) if rest.sum() > 0 else float("inf")

print(f"tone {tone:.0f} Hz at {rate:.0f} Hz")
print(f"  SFDR            {sfdr:6.1f} dB   worst spur at {freq[j]:.0f} Hz")
print(f"  tone / all else {thdn:6.1f} dB")
