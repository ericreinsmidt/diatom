# The GBA stutter was a log file

Reported by Eric as "the game and audio stutter terribly" on Ninja Five-0.
Found 2026-08-28. It was not audio, and it was not GBA.

## Cause

mGBA logs **every DMA transfer** at `RETRO_LOG_INFO`. `core_log` in `src/env.c`
had no level filter at all - everything below `WARN` became `DIATOM_LOG_INFO`
and went straight out.

Under the launcher, Diatom's stdout is `playos.log` **on the SD card**. So that
is thousands of card writes a second during play. One of Eric's sessions left
**90,273 lines and 5.6 MB** behind.

## Proof

One variable changed, same ROM, same display mode, same everything else:

| output goes to | fps (target 59.7275) | resyncs | dropped | queue min |
|---|---|---|---|---|
| discarded | 59.73 | 0 | 0 | 445 |
| a file on the SD card | **43.18** | **68** | **24** | **0** |

A queue minimum of zero is a real underrun, which is the audible gap. After the
fix, under the *same* card-writing conditions: **59.73 fps, 0 resyncs, 0
dropped, queue min 378**, and the log is 18 lines rather than 4714.

## Why it survived a nine-run audio survey the day before

**Every test in that survey sent output through adb to a pipe.** A pipe is
cheap and an SD card is not, so the defect was invisible to the entire harness.
The survey concluded GBA was clean; the player said it stuttered; both were
correct about different configurations and neither could see the other's.

What made it findable was one sentence from Eric: *"The audio sounded fine when
you played it. It stuttered like crazy when I played it myself."* That is not a
bug report, it is a statement that the difference is the harness rather than the
game, and it is the only reason the search went anywhere.

The general lesson is the one this project keeps paying for: **a test harness
that differs from the deployed configuration will certify the deployed
configuration as working.** The instruments were all correct. They were pointed
at something else.

## Two wrong theories first, both killed by measurement

**The resample ratio.** GBA was written into the register on 2026-08-27 as "the
awkward ratio, 65536 into 48000", and a run on 2026-08-28 reported 48000 into
48000 instead, which looked like a clean refutation.

**Do not trust either reading.** The 48000 came from
`/mnt/SDCARD/PlayOS/cores/mgba_libretro.so`, and that binary is **not** the one
CORES.md pins - checked afterwards, it is `b2b8a57f` against a pinned
`abde7a07`. That is the identical mistake the §6 rate note already records from
2026-08-25, made again by someone who had read the warning. Three of the five
cores on the device diverge from their pins; see §15.

What survives is only that the ratio was not the cause of the stutter.

**Launcher CPU contention.** `playos.elf` uses 29% of a core at the shelf, which
looked damning. Sampled through a real launch: it drops to **0%** the moment the
game starts. Not it.

Neither cost much, because both were checked rather than argued. The third
theory was found by looking at the log file that the second theory made me open.

## Fix

Core `DEBUG`/`INFO` is dropped once the game starts. Load-time `INFO` is kept -
that is where a core states its version, which is worth having - and
`DIATOM_CORE_LOG=1` restores everything for anyone debugging a core.

## Still open

The SNES half of the original report was never reproduced. The survey found
Parodius and Star Fox clean, and the chatty core here is mGBA specifically -
nothing in the log came from `snes9x2010`. If SNES stutters it is a different
fault, or it was this one seen through a different game.
