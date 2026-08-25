# Spike result - what a warm launch actually costs

- **Date:** 2026-08-25
- **Question:** *ADR-0006 and ADR-0008 exist to make launches fast. Cold launch
  is 625-750 ms. What does a launch cost when the process is already up and the
  core is already resident?*
- **Status:** Answered. **~35 ms for NES, ~200 ms for a 32 MB GBA title.**
- **Instrument:** [`tools/warmprobe.c`](../../tools/warmprobe.c)

## Why it mattered

Two accepted ADRs rest on this. ADR-0006 keeps every core resident and never
calls `dlclose`; ADR-0008 makes Diatom a long-lived process. Both are justified
by launches being fast in the steady state - and that had never been measured.
The protocol work in ADR-0009 is what delivers the steady state, so the number
belonged before the implementation rather than after.

## Method

The real sequence a resident frontend performs: `dlopen` and `retro_init` once,
then per game `read ROM -> retro_load_game -> frames -> retro_unload_game`, with
no `dlclose` and no `retro_deinit` between games.

Several ROMs per core, including repeats, so cache effects are visible.

## Result

**One time, and residency removes it entirely:**

| Core | `dlopen` (warm) | `retro_init` |
|---|---|---|
| fceumm | 6.0 ms | 0.0 ms |
| mgba | 5.5 ms | 0.0 ms |
| snes9x2010 | 0.7 ms | 2.6 ms |

**Per game, which is what a warm launch costs:**

| | NES (fceumm) | SNES (snes9x2010) | GBA (mgba) |
|---|---|---|---|
| ROM read | 0.3-3.3 ms | 1.5-4.4 ms | 11.5-36.5 ms |
| `retro_load_game` | 5.2-9.4 ms | 11.7-22.0 ms | 56.6-113.4 ms |
| `retro_unload_game` | 0.2 ms | 0.0 ms | 1.8-10.0 ms |
| **subtotal** | **6-13 ms** | **13-26 ms** | **68-150 ms** |

Adding the 3-frame warmup (~30 ms, measured separately) gives a realistic warm
launch of **~35 ms for NES** and **~200 ms for a 32 MB GBA title**, against
625-750 ms cold.

**A 20x improvement for NES, about 4x for the heaviest GBA game.** The win
shrinks as ROMs grow, because reading the file and `retro_load_game` are the two
costs residency cannot remove. Pokemon Emerald spends 110 ms inside
`retro_load_game` alone, on top of 36 ms of file read.

## Consistency with the earlier dlopen spike

The [RSS and dlopen spike](2026-08-23-rss-and-dlopen.md) measured fceumm at
**42.9 ms cold, 4.3 ms warm**, and 232 ms cold for all six cores together. The
6.0 ms here is one core warm and agrees with it. Worth stating because "232 ms"
is easy to misquote as one core's cost when it is the sum of six.

## Unplanned result: the first frame-budget breakdown

Timing 60 `retro_run` calls gives the core's own CPU cost per frame, which had
never been separated from the blit:

| | Core | Blit (`present`) | Total | Headroom of 16.6 ms |
|---|---|---|---|---|
| NES | 1.65 ms | 8.4 ms | 10.1 ms | 6.5 ms |
| SNES | 2.0-2.6 ms | 8.4 ms | ~11 ms | ~5.6 ms |
| GBA, Boktai (worst seen) | 4.2 ms | 8.4 ms | 12.6 ms | 4.0 ms |

**Diatom's own pixel loop costs two to five times more than emulating the
machine.** Every core in the matrix is cheap; the frontend is the expensive
part.

That reframes the disp2 hardware scaler, which ADR-0013 recorded as an escape
hatch for when CPU blitting stops fitting. It is not a contingency for heavy
cores - it is **the single largest saving available anywhere in the frame**, and
the only one worth more than a millisecond or two.

## Consequences

- **ADR-0006 and ADR-0008 are justified by measurement**, not assumption.
- **ADR-0009's protocol is the thing that delivers this**, which settles the
  order of work: the protocol before core options.
- **Per-launch cost tracks ROM size**, confirming the earlier finding from a
  different angle. NES is effectively instant; GBA is not, and no amount of
  frontend architecture changes that.
- **The hardware scaler deserves promotion** from escape hatch to the leading
  performance item.
