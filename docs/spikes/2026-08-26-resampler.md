# Spike result - the resampler, and a metric that scored the wrong winner

- **Date:** 2026-08-26
- **Question:** *`audio.c` called its linear interpolation "not good enough to
  ship". Is that true, and what replaces it?*
- **Status:** Answered. **True, and worse than the register thought.** Replaced
  with a 32-tap polyphase windowed sinc: **+28.5 dB** mean SFDR across seven
  rate pairs, for **+0.16 ms** per frame of a 16.6 ms budget and no new
  dependency.
- **Instruments:** [`tools/resampleprobe.c`](../../tools/resampleprobe.c),
  [`tools/spurs.py`](../../tools/spurs.py),
  [`tools/hfprobe.py`](../../tools/hfprobe.py)

## The measurement that was wrong

The register said **"No audible problem has been measured"**, and pointed at
drift and resync counts as evidence. Those measure whether the *rate control*
holds, which it does. They say nothing about what the *filter* does to a signal.

The first attempt to fix that measured Contra through `--tap-audio` and compared
energy above 10 kHz on each side. It found 17.5% of 50 ms windows leaving with
more HF than they arrived with, which a lowpass cannot do, and concluded
imaging. That conclusion was right.

Then the same metric was pointed at the new filter and reported **25%** - worse.
Taken at face value it says the windowed sinc is the inferior filter.

**It is not, and the metric is the reason.** Average spectra, same recording:

| band | linear - IN | sinc - IN |
|---|---|---|
| 5-10 kHz | -0.18 dB | +0.45 dB |
| 10-15 kHz | -0.64 dB | +0.79 dB |
| 15-20 kHz | **-2.49 dB** | +0.73 dB |
| 22-24 kHz | -4.38 dB | -9.49 dB |

Linear interpolation has a sinc-squared response: it **droops**. It was scoring
well on "fraction of energy above 10 kHz" by throwing away the treble that
belonged there. Losing signal and not adding artefacts look identical to a
single band ratio, and this one could not tell them apart.

The lesson is not about audio. **A metric that cannot separate "did less harm"
from "did less" will pick whichever is quieter.**

## The measurement that works

A game is a bad test bench because nobody knows its spectrum. A tone is a good
one, because the input has exactly one component and everything else in the
output was manufactured by the filter.

`resampleprobe` links `src/audio.c` itself - not a copy that could drift - stubs
the two port calls it makes, and pushes a sine at a chosen ratio. It reports a
steady half-full queue so rate control sits still and the filter is what is
being measured.

**SFDR, worst spur relative to the tone, dB below. Bigger is better.**

| source → dest | tone | linear | sinc | gain |
|---|---|---|---|---|
| 32040 → 48000 (NES) | 3 kHz | 41.6 | **77.2** | +35.6 |
| 32040 → 48000 | 8 kHz | **21.7** | **68.0** | +46.3 |
| 32768 → 48000 (Game Boy) | 5 kHz | 32.6 | 72.3 | +39.7 |
| 44100 → 48000 | 1 kHz | 67.3 | 79.1 | +11.8 |
| 65536 → 48000 (mGBA) | 3 kHz | 55.9 | 84.1 | +28.2 |
| 65536 → 48000 | 8 kHz | 36.2 | 74.4 | +38.2 |
| 48000 → 48000 (exact) | 3 kHz | 92.5 | 92.5 | 0.0 |

**21.7 dB is not a subtle defect.** It puts the loudest artefact 8% below the
tone it came from. The register's "no audible problem has been measured" was
true only in the sense that nobody had looked.

The last row is both filters hitting the 16-bit quantisation floor, which is the
control: at a ratio of exactly 1.0 there is nothing to resample and neither
filter can be blamed for what is left.

And the passband, 32040 → 48000, relative to 1 kHz:

| tone | linear | sinc |
|---|---|---|
| 5 kHz | -0.67 dB | **0.00** |
| 8 kHz | -1.79 dB | **0.00** |
| 11 kHz | -3.48 dB | **0.00** |
| 14 kHz | -5.82 dB | -2.12 dB |
| 15.5 kHz | -7.27 dB | -13.59 dB |

Flat where it should be flat, rolling off approaching the source Nyquist at
16020 Hz, where it must. The old filter was **3.5 dB down at 11 kHz** - audible
dulling, present on every system whose rate is not the device's.

## What was built

32 taps, 512 phases, Blackman window, cutoff at 0.92 of the lower Nyquist.
Hand-rolled rather than libsamplerate: the licence is compatible since 0.1.9,
but the Brick sysroot carries SDL2 and nothing else, so it would have meant
cross-building a dependency to replace eighty lines.

Both tables are **static**. §11's three-allocations-per-process property is
worth more than 98 kB of BSS, and `tools/conform-device.sh` still reports
`total=3` after the change.

Two decisions inside it are worth stating because the obvious alternative is
wrong:

**A real history buffer, not a carried sample.** A 32-tap filter needs 16
samples of lookahead. Clamping at block boundaries the way the old `tap()` did
would corrupt about 3% of samples at a 60 Hz rate - a buzz, not a rounding
error.

**No pass-through path at all.** A FIR delays by half its length and a copy does
not, so crossing between them jumps the stream by 16 samples: a click. FCEUmm
reports 48000 into a 48000 device, so its nominal ratio is exactly 1.0 and rate
control walks the real one back and forth across that boundary continuously. The
old code had a narrower form of this bug and patched it by carrying one sample
across the seam. Removing the seam removes the class. It costs a 22 kHz lowpass
and 64 multiplies per frame at ratio 1.0, which is not worth a mode switch.

## Cost

Measured on the device, Contra, 900 frames, two runs each: **554 and 560 CPU
ticks before, 573 and 570 after**. About +0.16 ms per frame against 16.6 ms, or
1% of the budget, for a mean 28.5 dB. Peak RSS 8.4 → 9.0 MB, still a third of
the 24 MB budget.

## Revisit if

- A core appears whose rate makes the transition band audible. 32 taps roll off
  from about 0.9 of the source Nyquist; more taps buy a narrower transition at
  linear cost, and the table is already generated rather than written out.
- Anyone hears the 2 dB dip at 14 kHz on NES content. That is the `ROLLOFF`
  constant, and trading it against stopband depth is a one-line change - but it
  is a judgement about listening, and this project has been wrong before by
  settling those from a table.
