# 0024. A session carries its persistence paths, and PREVIEW finally exists

- **Status:** Accepted
- **Date:** 2026-08-26
- **Supersedes:** -
- **Superseded by:** -

Extends [ADR-0009](0009-launcher-protocol.md) (new RUN keys, which its
ignore-unknown-keys rule was built for) and implements the part of
[ADR-0016](0016-saves-and-save-states.md) that said *"the launcher passes
explicit paths"* without saying how they arrive.

## Context

PlayOS is adopting Diatom, and its shelf reads
`.minui/<folder>/<base>.9.bmp` - the frame the player was looking at when they
stopped - to draw on a game's card. minarch writes that file; `PREVIEW` sat in
ADR-0009's message table unemitted. Separately, ADR-0016 decided states take
paths rather than slot numbers, and the resume flow (load a state at launch if
one exists, write one on every way out) had CLI flags but no protocol carrier.

## Decision

**Three optional keys on `RUN`, one emission rule, one new verb.**

```
RUN core= rom= tag= resume=<p> exit_state=<p> preview=<p>
```

- `resume=` is **load-if-exists**: a missing state is a fresh start, not an
  error, so a launcher passes it unconditionally and never stats the file
  itself.
- `exit_state=` is written on every way out - STOP, QUIT, SIGTERM.
- `preview=` is a BMP of the frame, written at two moments: **entering pause**
  (before `PAUSED` goes out, so the launcher's menu has its backdrop the moment
  it owns the display - the order is the contract) and **exit** (before `EXIT`,
  with `PREVIEW path=` announced after the write, so the file is on disk before
  the launcher redraws its cards).

**The preview is the core's frame, not the screen.** A 256x224 frame is 8-30x
smaller than the 1024x768 panel, never contains the OSD bar, and needs nothing
from the port - the host already holds the last frame for dupe handling. A
launcher decodes one preview per visible card; size is the difference between a
shelf that scrolls and one that stutters.

**`RESET`** runs `retro_reset()` and answers `RESETDONE`, legal while running
or paused. It exists because minarch's menu has a Reset row and the launcher's
menu (ADR-0016) needs to offer the same without stopping and relaunching.
Standalone CLI parity: `--preview-on-exit`.

## Consequences

**Easier.** The PlayOS migration's Phase 1 requirement is met: a session driven
over the socket leaves the exact artifacts minarch left, at launcher-chosen
paths, in a quarter of the bytes.

**Found on the way, both now fixed in the Makefile.** Changing `diatom_core`'s
layout with no header dependency tracking left a stale `save.o` calling
`retro_reset` where it meant `serialize_size` - a state save that failed
silently after every fresh file was correct. And the dependency include, placed
before `all:`, made a generated rule the default goal, so `make` built one
object and stopped while looking exactly like the staleness it was added to
prevent.

## Revisit if

- A launcher wants the preview at a different size than the core's frame,
  which would argue for a `preview_w=`/`preview_h=` pair rather than a second
  file format.
- Anything wants `PREVIEW` on demand mid-game (a live thumbnail), which is a
  different feature: this one is deliberately tied to pause and exit.
