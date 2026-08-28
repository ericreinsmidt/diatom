# 0011. Lock the display rect at load from base geometry

- **Status:** Superseded by [0021](0021-settle-the-rect-before-locking.md)
- **Date:** 2026-08-23
- **Supersedes:** -
- **Superseded by:** [0021](0021-settle-the-rect-before-locking.md)

## Context

[ADR-0007](0007-port-interface.md) recorded the handling of frames that do not
integer-scale as a **default, not a decision**, with the note that *"whether that
looks acceptable is a judgment to make on hardware, not in a document."*

That judgment is now possible. The first working build reproduced the case on
its first run.

Cores announce hires by calling `SET_GEOMETRY` mid-run with a larger
`base_width`. Measured in the env-inventory spike: **3 of 6 cores do this** -
Snes9x reports `max 604x478` against `base 256x224`, Beetle PCE reports
`max 512x243`. It is a normal thing for a game to do when it opens a menu.

Recomputing the largest integer factor per frame produces this: at 256x224 on a
960x720 surface the factor is 3 and the picture is 768x672. At 512x224 the
factor is `960/512 = 1`, so the picture becomes 512x224 - **a fifth of the
area** - and snaps back when the mode ends. Captured and looked at; it is not
acceptable.

## Options considered

### Option A - recompute the integer factor per frame
What ADR-0007 defaulted to. Every frame integer-scales, but the picture changes
size dramatically mid-game. Rejected on sight of it.

### Option B - compute the factor from `max_width`/`max_height`
The rect never changes, and every frame integer-scales. But the common case is
then sized for a mode it rarely uses: SNES base 256x224 against max 604x478
would display at 1x, wasting most of the screen permanently to accommodate a
mode that appears in menus.

### Option C - lock the rect at load from base geometry
The rect is computed once from the geometry `retro_get_system_av_info` reports
and does not move. Frames of any size scale into it. Hires frames scale
non-integer horizontally - 512 into 768 is 1.5x.

### Option D - accept Option A
Defensible only if hires modes turn out to be rare and brief in the actual
library. Not knowable without playing the games, and the failure is ugly enough
that the burden of proof sits the other way.

## Decision

**Option C.** The display rect is computed once, from base geometry, at load.
`SET_GEOMETRY` is noted but does not move it.

The picture stays the same size and position for the whole session.

**This is also more faithful than the alternatives.** SNES and PC Engine hires
pixels are physically **half-width** on original hardware - that is what the mode
is. Putting 512 columns in the screen width 256 occupied reproduces the original
geometry. Option A does not merely look worse, it is wrong: it renders
half-width pixels as full-width ones and shrinks the frame to compensate.

## Consequences

**Easier:** Constant picture size and position. The rect is computed once rather
than per frame, so the frame loop does no geometry arithmetic at all - which
suits the per-game/per-frame discipline in register §2.

**Harder - and this cost is observed, not predicted:** hires frames scale
**non-integer** horizontally at 1.5x. Single-pixel features alternate between one
and two screen pixels under nearest-neighbour sampling.

`test/stubcore.c` draws a patch of one-pixel vertical lines specifically to
expose this. At base geometry they render as clean 3px stripes; at hires they
moiré visibly. On real content the likely victim is text in a hires menu - which
is, unhelpfully, exactly what hires modes are usually *for*.

The trade is therefore: a constant, correctly-proportioned picture with an
artifact on fine detail, versus an integer-clean picture that changes size by 5x
mid-game. Having looked at both, the first is clearly better - but it is a
trade, not a free win.

**It qualifies the integer-only principle.** ADR-0005's rule 4 asks that a system
integer-scale cleanly on a 4:3 panel; that remains true of every in-scope
system's *base* geometry, which is what the rule is about. But Diatom no longer
integer-scales every *frame*. Worth stating plainly rather than letting the two
documents quietly disagree.

**Assumes hires is the reason base geometry changes.** PC Engine genuinely
switches between several horizontal widths - 256, 336 and 512 - and not all of
those are a hires mode. A core changing width for some other reason will be
stretched into the locked rect rather than resized. Untested against real PC
Engine content.

## Revisit if

- A core changes base geometry for a reason other than hires and the result
  looks stretched - PC Engine's 336-wide mode is the specific thing to check; or
- non-integer hires scaling proves visibly bad on real content, in which case
  Option B becomes worth its wasted screen area.
