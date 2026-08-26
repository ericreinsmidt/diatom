# 0021. Lock the rect, but relock if what we locked onto was never real

- **Status:** Proposed
- **Date:** 2026-08-26
- **Supersedes:** [0011](0011-lock-the-display-rect.md)
- **Superseded by:** -

Replaces [ADR-0011](0011-lock-the-display-rect.md), whose decision was right for the
case it was written from and wrong for a case it did not have.

## Context

ADR-0011 computes the display rect once, at load, from the geometry
`retro_get_system_av_info` reports, and never moves it. Its reasoning was sound:
cores announce hires by calling `SET_GEOMETRY` mid-run, recomputing an integer
factor from the new width collapses 3x to 1x, and the picture shrinks to a fifth
of its area and snaps back. That was looked at on hardware and rejected.

The assumption underneath it is that **the geometry reported at load is the mode
the game runs in**, and that a later change is an excursion.

`retro_get_system_av_info` is called immediately after `retro_load_game`, before
a single `retro_run`. The core has emulated nothing at that point, so it reports
its reset state - which for some systems is not a mode any game is ever seen in.

Measured 2026-08-26 with [`tools/envlog.c`](../../tools/envlog.c), 3600 frames,
recording every distinct frame size and the frame it first appeared on:

| core | content | timeline |
|---|---|---|
| `genesis_plus_gx` | Herzog Zwei | 256x192 for **29 frames**, then 320x224 for 3571 |
| `genesis_plus_gx` | Phantasy Star IV | 256x192 for **1 frame**, then 320x224 for 3599 |
| `genesis_plus_gx` | Sonic (Master System) | 256x192, stable |
| `genesis_plus_gx` | Shining Force (Game Gear) | 160x144, stable |
| `snes9x2010` | four games | 256x224, stable |
| `mednafen_pce_fast` | three games | 256x243, stable |

**The geometry ADR-0011 locks from lasts between one and twenty-nine frames out
of 3600 on Genesis.** It is not a mode, it is a boot artefact. Master System and
Game Gear through the *same core* are stable, so this is a property of the
system, not of the core.

The visible cost, `integer` mode on Herzog Zwei: the rect locks at 1024x768 from
256x192, and the 320x224 frames that actually arrive scale by **3.20x
horizontally and 3.43x vertically**. Not integer, not even uniform - which is
the one thing that mode's name promises.

## Options considered

**Wait before locking.** Run N frames, then lock from what the core actually
delivered. Correct, and it puts N frames of latency on every launch. Warm launch
is 35 ms and Herzog Zwei settles at frame 29, so this would add half a second to
the number the whole resident architecture exists to protect. Rejected.

**Lock from `max` geometry.** ADR-0011's Option B, rejected there for the same
reason it is rejected here: SNES base 256x224 against max 1024x478 would display
at 1x and waste most of the panel permanently.

**Relock on any geometry change.** ADR-0011's Option A. This is exactly the
5x-resize-mid-game behaviour that ADR-0011 was written to prevent, and nothing
in the new measurement makes it less bad.

**Discriminate on what changed** - width-only is hires, height too is a mode
change. Fits PC Engine (256→512, height constant) and Genesis (both change), but
breaks on interlaced SNES modes, which double height and *are* the excursion
kind. A rule that reads the values is guessing at intent.

## Decision

**Keep the lock. Relock only if the geometry being displayed turns out not to
have held.**

    if (geometry changed && frames since we locked < SETTLE_FRAMES)
            recompute the rect from the new geometry

`SETTLE_FRAMES` is 180, three seconds. The slowest correction observed is frame
29, so that is two orders of margin, and it is still far short of anything a
person reaches by opening a menu.

**The discriminator is not what changed, it is whether what we locked onto ever
really held.** That is the question ADR-0011 was actually asking and answered by
assumption. A boot mode corrects itself within half a second, during the logo. A
hires menu opened minutes into a game does not qualify, the rect stays put, and
ADR-0011's behaviour is preserved exactly where it was right.

It is also self-correcting: a core that changes geometry several times while
booting relocks each time until one persists.

## Consequences

**Fixed, and measured on hardware.** `integer` on Herzog Zwei now locks 960x672
for 320x224 frames - **3.00x on both axes**, where it was 3.20x/3.43x.

**PC Engine was wrong too, and less visibly.** Under Diatom, mednafen_pce_fast
settles 256x243 → 256x240 at frame 1. The old rect was 768x**729**, computed for
a height the core stops using, so 240-tall frames scaled by 3.0375x. It is now
768x720, exactly 3x. Two of the five pinned cores were affected.

**`docs/reference/core-facts.md` was generated from the boot mode, and so was
part of ADR-0018.** `tools/corefacts.sh` ran 30 frames and took the *first*
geometry line. Genesis was therefore recorded as **256x192 at aspect 1.5238**,
which is twenty-nine frames of boot; it actually runs **320x224 at 1.3061**.
That table is what [ADR-0018](0018-integer-vertical-remeasured.md) computed its
display-mode comparison from, so **ADR-0018's Genesis row describes a mode no
game runs in**. ADR-0018 is Accepted and immutable; this records the fault
rather than editing it. `corefacts.sh` now runs 180 frames and takes the last
line, and Diatom reprints the geometry in the same format once it settles.

There is an unpleasant symmetry here worth naming: ADR-0018 exists **because**
ADR-0015's table had gone stale, and its fix was to generate the table by
measurement instead of transcribing it. The generated table was then wrong for a
different reason - measured too early rather than copied too late. Generating a
number does not make it true; it only moves where the error can hide.

**An instrument disagreed with the frontend, and the frontend was right.**
`envlog` shows PC Engine stable at 256x243 because it declines core options;
Diatom answers them, and mednafen_pce_fast's scanline options change its height.
An instrument that models less than the real program can report stability the
real program does not see.

**Harder.** The rect can now move once, early. On Genesis the picture resizes
during the boot logo. That is a real visible event and it is the price of not
being wrong for the rest of the session.

## Revisit if

- A core is found that settles later than `SETTLE_FRAMES`, which would show as
  the wrong rect for the whole session and is the failure this cannot detect on
  its own. `tools/envlog.c` with a large frame count is the check.
- A game legitimately changes mode within three seconds of loading and the
  resize is judged worse than the mis-scaling it prevents. That needs eyes.
- Anything makes the boot-time resize objectionable on hardware, in which case
  the wait-before-locking option returns with a launch-latency cost to weigh.
