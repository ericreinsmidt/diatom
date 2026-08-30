# 0027. An overlay the port composites, because only one process can present

- **Status:** Proposed
- **Date:** 2026-08-29
- **Supersedes:** -
- **Superseded by:** -

Extends [ADR-0007](0007-port-interface.md) (the port interface, and the seam
this adds a function to) and [ADR-0009](0009-launcher-protocol.md). Exists
because of [ADR-0026](0026-achievements-on-the-launcher-protocol.md): an
achievement unlocks mid-game and nobody could say so.

## Context

Diatom owns the display while a game runs. That is ADR-0009's central rule and
[the handoff spike](../spikes/2026-08-24-display-handoff.md)'s one invariant:
**one presenter at a time**, because two of them wedged the display engine
in-kernel and needed a power cycle to clear.

So when ADR-0026 landed and achievements started firing, the launcher had no
way to mention it. It knew, immediately - Diatom sends `CHEEVO id= state=` the
moment a condition fires - and could do nothing with it but write it down. The
player found out by opening the menu.

**Diatom can already draw over a running game**, and does. `port/brick.c`
composites a level bar straight into the page after the blit and before
publish, riding the same flip, whenever the volume or brightness OSD timer is
running. The mechanism is not missing. What is missing is anything to put in
it: Diatom has no font, no text layout, and no opinion about wording, and none
of those belong to an emulator frontend.

The launcher has all three, and cannot present.

## Options considered

### Option A - Diatom renders the text

A font, a face to ship, layout, and wording. Every one of those is a thing the
launcher already owns and Diatom has spent twenty-six decisions not acquiring.
It also puts the words in the wrong repository: the launcher knows what an
achievement is called, and Diatom deliberately does not know what an
achievement is.

### Option B - Diatom pauses and hands the display back for a moment

Correct by the invariant and awful to play: the game would stop for four
seconds to tell you that you did well.

### Option C - the launcher renders pixels, the port composites them

Text stays where the fonts are. Diatom gains "blit this image at this place for
this long", which is the job it already does for the level bar.

## Decision

**Option C.** One new port function:

```c
void diatom_port_overlay(const uint8_t *bgra, int w, int h, unsigned ms);
```

and one new protocol message, `OVERLAY path= ms=`, answered with `OVERLAID` or
`ERROR code=bad_overlay`.

**The name is load-bearing.** ADR-0007 says the port deals in pixels, samples,
buttons and time, and that *"if a function name here grows a domain noun -
game, save, core, menu - it is in the wrong layer"*. `overlay` is a pixel word.
`notice`, `achievement` or `message` would each have been this header's own
rule being broken, and the parameters are pixels and a duration for the same
reason.

**The pointer is borrowed, not copied.** The host holds one buffer and
guarantees it stays valid until the next call or until the duration elapses.
A port that copied would need its own, sized for the largest notice anyone
might send - roughly 150KB per port on a device where the whole process is
meant to hold 8.2 MB (§11).

**The file format is not BMP**, which was the obvious choice and the wrong one.
Alpha through BMP means `BI_BITFIELDS`, SDL will not write it, and a notice
with no alpha is a hard rectangle stamped over the game. It is eight bytes and
then rows:

```
"DTOV"  uint16 width LE  uint16 height LE  w*h*4 BGRA
```

A private channel between two programs in one repository pair, written and read
within milliseconds. Worth being exactly what is needed, and worth saying so
rather than bending a standard until it fits.

**An image larger than the surface is refused, not clipped.** Something drawn
half off the panel is a bug that looks like a design choice.

## Consequences

The launcher can say things during a game without ever presenting, which was
the whole problem. Anything it can render, it can show - the achievement notice
is the first user, not the only possible one.

`make check-seam` is unaffected: this adds no include and no domain noun.
`test/stateplane.py` covers the carrying rather than the look - accepted while
a game runs, a missing file refused, a file that is not an overlay refused,
`ms=0` clears, and the game still ends normally afterwards.

The look is judged by looking. TortOS renders the notice through its own shot
harness (`--notice`), and the first version put "Achievement unlocked" in
larger type than the achievement's name - obvious in one screenshot and
invisible for as long as it was only being reasoned about.

**Not addressed:** more than one overlay at once. A second call replaces the
first, so two achievements unlocking within four seconds shows only the second.
Real, and not worth a queue until it happens to someone.

## Revisit if

- Something wants an overlay that changes while it is up - progress, a
  countdown - which this cannot do, because the host hands over pixels once.
- A port appears that cannot composite in screen space, which would mean the
  assumption that the port owns the blit no longer holds.
