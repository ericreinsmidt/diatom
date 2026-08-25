# Spike result - PicoDrive vs Genesis Plus GX

- **Date:** 2026-08-25
- **Question:** *Which core should cover the Sega block, and does a second core
  for one system prove the seam?*
- **Status:** Answered. **Genesis Plus GX**, on correctness, if 32X is out of
  scope.
- **Cores:** `PicoDrive 2.05-733c711`, `Genesis Plus GX v1.7.4 b7e79b3`, both
  from libretro's buildbot, hashes in [CORES.md](../../CORES.md).

## Why it mattered

Nine systems were covered by six cores, with Master System and Game Gear
assigned to none. Both candidates cover Genesis, Master System, Game Gear and
SG-1000 from one binary, so either would take the count to five cores for nine
systems. libretro's docs call PicoDrive *"designed to run on weak devices"* with
*"lower accuracy than Genesis Plus GX"*, so the expectation was a speed-against-
accuracy trade.

**It was not that.** On this hardware both hold frame rate perfectly, and the
difference is what they tell the frontend.

## What they report

Five games, three systems, 300 frames each. **Every run: 0 resyncs.**

| System | PicoDrive | Genesis Plus GX |
|---|---|---|
| Master System | 320x240 @ 60.0000 | **256x192** @ 59.9227 |
| Game Gear | 320x240 @ 60.0000 | **160x144** @ 59.9227 |
| Genesis | 320x240 @ 60.0000 | 256x192 (max 348x240) @ 59.9227 |

**PicoDrive reports a fixed 320x240 for every system.** Genesis Plus GX reports
each system's true geometry, including Game Gear's actual 160x144 LCD. Its
59.9227 is also the correct NTSC rate; 60.0000 is rounded.

## What that does to the picture

Because the display rect is computed from reported geometry, the difference is
visible, not academic:

| | PicoDrive | Genesis Plus GX |
|---|---|---|
| Game Gear, integer | 960x720 from a claimed 320x240 | **800x720** = exactly 5x of 160x144 |
| Master System, integer | 960x720 | **1024x768** = exactly 4x of 256x192, **the whole panel** |

Game Gear is the clear case. PicoDrive's 160x144 content is scaled into its own
320x240 canvas and then scaled again by Diatom, so it arrives visibly
distorted. Genesis Plus GX gives a clean 5x with correct proportions. Master
System is nearly as stark: 4x of 256x192 is 1024x768, an exact fill of this
panel with nothing wasted, where PicoDrive leaves borders on all four sides and
a wrong shape inside them.

## What it costs

| | PicoDrive | Genesis Plus GX |
|---|---|---|
| Core binary | 1.9 MB | **12.6 MB** |
| Present cost | 8.38-8.42 ms | 8.57-8.69 ms |
| Save state, Master System | 75 KB | **1,036 KB** |
| SRAM, Phantasy Star (SMS) | 32 KB | **64 KB** |
| Serialization quirks | none | **0x40** PLATFORM_DEPENDENT |

Genesis Plus GX allocates a fixed ~1 MB state regardless of system, so a Master
System state is 14x larger than PicoDrive's. Against the measured write curve
that is roughly 130 ms per state write - comfortably inside the ~810 ms the
device gives after SIGTERM, but worth knowing.

It is also the first core in the matrix to declare any serialization quirk.
`PLATFORM_DEPENDENT` means its states cannot move between architectures, which
matters if states are ever synced between a device and a desktop.

## Decision

**Genesis Plus GX**, unless 32X is in scope - PicoDrive is the only one of the
two that has it. Nothing about performance argues for PicoDrive here; both hold
frame rate with room, and Diatom's own blit dominates either way.

## The seam held

An unfamiliar core, three systems it had never been run against, five games:
zero code changes and zero resyncs. That closes the register's standing item to
verify against a second core for one system - and the value came precisely from
how much the two cores disagreed. Any assumption Diatom had quietly inherited
from PicoDrive's reporting would have surfaced here.

## Saves do not transfer between cores

The same cartridge reports 32 KB of SRAM under one core and 64 KB under the
other, with state formats that share nothing. ADR-0016's header already refuses
a foreign state. SRAM falls back to `min(file, sram)` with a warning, which is
the right default, but a launcher offering per-system core choice needs to treat
a core change as a save-compatibility break.
