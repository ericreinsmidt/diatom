# Spike result - which SNES core

- **Date:** 2026-08-25
- **Question:** *Mainline snes9x cannot run on the Brick. Is there an
  alternative that can, or must a C++ core be built?*
- **Status:** Answered. **`snes9x2010`**, and nothing needs building.

## Why it was in doubt

SNES was the only system with no working core: mainline `snes9x` is C++ and
wants `GLIBCXX_3.4.29`, while the Brick ships `libstdc++.so.6.0.28` topping out
at `3.4.28`. Building it in the ADR-0012 container was the assumed fix. It is
not needed.

## Filtering by runtime, not by reputation

Eight candidates fetched from libretro's buildbot, checked for the C++ runtime
version they demand:

| Core | Max GLIBCXX | Runs here |
|---|---|---|
| `snes9x2002` | none, pure C | yes |
| `snes9x2005` | none, pure C | yes |
| `snes9x2005_plus` | none, pure C | yes |
| `snes9x2010` | none, pure C | yes |
| `bsnes_mercury_balanced` | 3.4.21 | yes |
| `snes9x` (mainline) | 3.4.29 | **no** |
| `mesen-s` | 3.4.30 | **no** |
| `mednafen_supafaust` | 3.4.29 | **no** |

`bsnes_mercury_balanced` was the surprise: C++, but built against a much older
runtime, so it loads.

## Measured on device, Parodius (PAL), 900 frames

| Core | Reports | Result |
|---|---|---|
| **`snes9x2010`** | 256x224, max **1024x478**, **50.0070 fps** | **50.01 fps, 0 resyncs** |
| `snes9x2005_plus` | 256x224, max 512x512, 50.3197 fps | 50.32 fps, 0 resyncs |
| `snes9x2005` | as above | 0 resyncs |
| `snes9x2002` | as above | 0 resyncs |
| `bsnes_mercury_balanced` | 256x224, max 512x478, 50.0070 fps, aspect 1.5842 | **36.56 fps, 78 resyncs** |

## The deciding difference is not speed

All four light forks hold frame rate comfortably. What separates them is that
**`snes9x2002`, `snes9x2005` and `snes9x2005_plus` report 50.3197 fps, while
mainline snes9x 1.63, bsnes and `snes9x2010` all report 50.0070.**

50.0070 is the correct PAL SNES rate. The 0.62% error is not cosmetic: Diatom
paces to the rate the core reports, so PAL content would run 0.62% fast with
audio pitched to match - roughly 22 seconds of drift per hour. That is an
accuracy difference visible in the one number a frontend is obliged to trust.

`snes9x2010` also declares the widest hires range of the group, max 1024x478.

## bsnes is out on measurement, not reputation

It reports the most faithful numbers of any candidate - the correct PAL rate and
a PAL-correct 1.5842 aspect where every snes9x fork says 1.3333 - and it cannot
run: 36.56 fps against a 50.0070 target with 78 resyncs. libretro's docs warn
that bsnes has *"minimum system requirements greater than with other
emulators"*, and on a Cortex-A53 that is decisive.

## Consequence

The test matrix is **five cores covering all nine in-scope systems**, all
fetched rather than built:

`fceumm` · `snes9x2010` · `mgba` · `genesis_plus_gx` · `mednafen_pce_fast`

The C++ build path in the ADR-0012 container remains available and is now
unused. That is worth knowing rather than forgetting: if a future system needs a
core that only exists as C++ against a modern runtime, the container is the
answer, and it has been verified to produce exactly `GLIBCXX_3.4.28`.
