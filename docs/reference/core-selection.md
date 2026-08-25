# Core selection - what was chosen, what was rejected, and why

**Date:** 2026-08-25. Companion to the
[survey](libretro-cores-survey.md), which catalogues what *exists*. This
records what was *chosen*, on what evidence.

## Read this first

**Diatom has no core list.** It loads whatever core it is handed, and every core
named here is a **test-matrix entry** - the set Diatom has been verified
against, so its geometry, pacing and memory claims have evidence behind them.
Which core covers which system is the **host application's** config decision;
for PlayOS that is `systems.cfg`.

So this document answers two different questions that are easy to conflate:

- *What is Diatom verified against?* The five cores below.
- *What should a host on this device class ship?* The same five, for the reasons
  given - but that is a recommendation, not a dependency.

Every binary came from libretro's own buildbot via `tools/fetch-cores.sh`, with
sha256 in [CORES.md](../../CORES.md). Versions below are what each core reports
through `retro_get_system_info`, logged at load.

---

## The matrix: five cores, nine systems

| Core | Reports itself as | Systems covered | Chosen because |
|---|---|---|---|
| `fceumm` | FCEUmm (SVN) 236ccdf | NES | Only NES core tested. Documented as low-resource; no reason found to look further |
| `snes9x2010` | Snes9x 2010 1.52.4 7db129b | SNES | The only pure-C snes9x fork that reports **correct** frame rates on both PAL and NTSC. See below |
| `genesis_plus_gx` | Genesis Plus GX v1.7.4 b7e79b3 | Genesis, Master System, Game Gear | Reports true per-system geometry where PicoDrive reports a fixed 320x240. See below |
| `mgba` | mGBA 0.11-219-e31759b | Game Boy, Game Boy Color, GBA | Covers three systems in one binary and never drops a frame |
| `mednafen_pce_fast` | Beetle PCE Fast v1.31.0.0 | PC Engine (HuCard), PC Engine CD | Covers both media from one binary |

**Cores are the cost unit, not systems.** Two of these five carry six of the
nine systems, which is why adding Master System and Game Gear cost nothing.

---

## Rejected, with the measurement that rejected it

Every one of these is a reasonable choice that lost on evidence rather than
reputation.

| Core | For | Verdict | The measurement |
|---|---|---|---|
| `picodrive` 2.05-733c711 | Sega block | **Rejected** | Reports a fixed **320x240 for every system**. Game Gear's real 160x144 is scaled into that canvas and then scaled again by Diatom, arriving visibly distorted; Genesis Plus GX gives an exact 5x. Both hold frame rate, so this was never a speed question |
| `gambatte` | Game Boy, GBC | **Redundant** | mGBA gives identical treatment - 160x144, aspect 1.1111, 59.7275 fps, exact 5x integer - from a binary **47% smaller**, opening a `.gb` ROM in the same 625 ms. Also C++, so it cannot load here at all |
| `gpsp` | GBA | **Unnecessary** | Covers GBA only, so it cannot replace mGBA, only add a sixth core. mGBA holds **59.73 fps with 0 resyncs** on Golden Sun, Pokemon Emerald, Boktai and Super Mario Advance 2 |
| `bsnes_mercury_balanced` | SNES | **Rejected** | Reports the most faithful numbers of any SNES candidate - correct PAL rate and a PAL-correct 1.5842 aspect where every snes9x fork says 1.3333 - and manages **36.56 fps with 78 resyncs** against a 50.0070 target |
| `snes9x2002` | SNES | **Rejected** | Holds frame rate, but reports **50.3197 fps** for PAL and **59.9227** for NTSC. Correct values are 50.0070 and 60.0985 |
| `snes9x2005` | SNES | **Rejected** | Same timing error |
| `snes9x2005_plus` | SNES | **Rejected** | Same timing error |

### Why misreported timing disqualifies a core

Diatom paces against a monotonic clock **at the rate the core reports**
(ADR-0011 and the frame loop). A core that claims 50.3197 fps for PAL content
running at 50.0070 will be paced 0.62% fast, with audio resampled to match, so
the game runs fast and slightly sharp in pitch - roughly 22 seconds of drift per
hour. On NTSC the same forks are 0.29% slow.

This is not a cosmetic difference. It is wrong in the single number a frontend
is obliged to trust, and it cannot be corrected downstream without second-
guessing the core.

---

## Blocked before merit is considered

The Brick ships `libstdc++.so.6.0.28`, topping out at `GLIBCXX_3.4.28`. Cores
built against GCC 11 or newer cannot `dlopen` at all, regardless of quality.

| Core | Demands | Result |
|---|---|---|
| `snes9x` (mainline) | GLIBCXX_3.4.29 | Cannot load |
| `gambatte` | GLIBCXX_3.4.29 | Cannot load |
| `mesen-s` | GLIBCXX_3.4.30 | Cannot load |
| `mednafen_supafaust` | GLIBCXX_3.4.29 | Cannot load |
| `bsnes_mercury_balanced` | GLIBCXX_3.4.21 | Loads (built against an older runtime) |
| every pure-C core | none | Loads |

**Checking `GLIBC_` does not catch this.** The C++ runtime versions its symbols
separately as `GLIBCXX_`, and a core can be clean on one and fail on the other.
That mistake was made here first and caught only when the core refused to load.

**The fix exists and is unused.** The ADR-0012 toolchain container has GCC
10.2.1, whose cross `libstdc++.so.6.0.28` provides exactly `GLIBCXX_3.4.28` -
the same version the device ships. A C++ core built there runs unmodified. No
core currently needs it, because `snes9x2010` closed the only gap.

---

## The numbers behind the choices

All on a TrimUI Brick, firmware 1.1.1, Cortex-A53.

| Measurement | Result |
|---|---|
| Every chosen core, sustained | Target frame rate, **0 resyncs** |
| PAL conformance (Probotector, 50.0070 target) | 50.01 fps, 0 resyncs, drift -0.284% |
| SNES coprocessors: SuperFX, SA-1, DSP-1, CX4 | All full speed, 0 resyncs, **on both candidate cores** |
| mGBA on four demanding GBA titles | 59.73 fps, 0 resyncs each |
| Resident cost, six cores mapped plus one running | 15.0 MB against 975 MB |
| Core `dlopen`, cold | ~230 ms, paid **once** per core (ADR-0006) |
| Per-launch cost | Tracks **ROM size**, not core choice: 625 ms for 32 KB, 750 ms for 16 MB |

Notably, **ADR-0005 predicted that SuperFX and SA-1 titles might not reach full
speed on a Cortex-A53**, naming Star Fox and Yoshi's Island. Both run at full
speed. The prediction is recorded as wrong as prominently as it would have been
recorded as right.

---

## Systems deliberately excluded

| System | Status | Reason |
|---|---|---|
| 32X | **Out** (2026-08-25) | Only PicoDrive supports it, and PicoDrive lost the Sega comparison - so it would mean a sixth core existing solely for a ~40 title library that is mostly Genesis ports |
| PC Engine CD | **In** | Needs a System Card BIOS. Does **not** need the Disk Control Interface: the Redump catalogue has 502 PCE CD entries and **zero** multi-disc titles |
| Sega CD | Unasked | `genesis_plus_gx` already covers it, but it is not free - 21 of its titles are multi-disc, so it brings the Disk Control Interface with it |
| Neo Geo, PS1, Saturn, N64 | Out | ADR-0005 rules 1, 2 and 3 |

---

## How to redo this

The method mattered more than any individual answer, and it generalises to a new
device:

1. **Filter by runtime first.** Fetch candidates and check the `GLIBCXX_` and
   `GLIBC_` versions they demand against what the device ships. This cut eight
   SNES candidates to five in one step, before quality entered the picture.
2. **Run them.** `tools/brick-run.sh --exec` keeps the display guarded; two
   processes presenting at once wedges the GPU firmware in-kernel.
3. **Read what the core reports, not just whether it runs.** Every rejection
   above except bsnes came from *reported values* - geometry, aspect, frame rate
   - not from dropped frames. A core that runs perfectly while lying about its
   geometry produces a wrong picture, and a frontend cannot detect that from
   frame timing.
4. **Test a second core for at least one system.** The disagreements are the
   point: two Sega cores differed on geometry, aspect, frame rate, SRAM size,
   state size and serialization quirks for the same cartridge. Anything Diatom
   had quietly inherited from one core's behaviour surfaced immediately.

## Consequences a host should know

**Saves do not transfer between cores.** The same Genesis cartridge reports 16 KB
of SRAM under PicoDrive and 64 KB under Genesis Plus GX, with incompatible state
formats. ADR-0016's state header refuses a foreign state; SRAM falls back to
`min(file, sram)` with a warning. A launcher offering per-system core choice must
treat a core change as a save-compatibility break.

**Genesis Plus GX declares `PLATFORM_DEPENDENT`** serialization quirks - its save
states cannot move between architectures. It is the only core in the matrix to
declare any quirk.

## Sources

- Measurements: [Sega core comparison](../spikes/2026-08-25-sega-core-comparison.md),
  [SNES core selection](../spikes/2026-08-25-snes-core-selection.md),
  [environment inventory](../spikes/2026-08-23-env-inventory.md),
  [RSS and dlopen](../spikes/2026-08-23-rss-and-dlopen.md)
- Field: [the survey](libretro-cores-survey.md)
- Provenance: [CORES.md](../../CORES.md), `tools/fetch-cores.sh`
- Scope: [ADR-0005](../decisions/0005-system-inclusion-criteria.md), and the
  register's systems table
