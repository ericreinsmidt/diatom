# 0022. Display mode joins the state plane, and is the second contested row

- **Status:** Accepted
- **Date:** 2026-08-26
- **Supersedes:** -
- **Superseded by:** -

Extends [ADR-0020](0020-shared-state-plane.md), which is unchanged, and is the
case its "Revisit if" named.

## Context

The display mode is process-wide. `--display` sets it at startup, the user can
cycle it with a chord, and `RUN` carries no mode field - so **a launcher cannot
choose a display mode per game or per system.** It is fixed for the life of the
process, or the user sets it by hand every session and it does not persist.

That was defensible while the modes looked interchangeable. Two measurements
say they are not:

**[ADR-0018](0018-integer-vertical-remeasured.md)** withdrew integer-vertical's
"never worse" guarantee: it is better than plain `integer` on NES, SNES NTSC and
PC Engine, and substantially worse on SNES PAL and Genesis. The right choice
already differed by system.

**[ADR-0021](0021-settle-the-rect-before-locking.md)** sharpened it. Herzog Zwei
at 320x224 on a 1024x768 panel, measured 2026-08-26:

| mode | panel used | uniform pixels | crops |
|---|---|---|---|
| stretch | 100% | no | no |
| aspect | 98% | no | no |
| integer | 82% | **yes** | no |
| integer-vertical | 75% | no | no |
| overscale | 100% | **yes** | yes |

Sixteen points of panel separate `integer` from `aspect` on Genesis, and the
trade lands differently on a system whose base geometry divides the panel
cleanly. That is a per-system preference, and §12 is explicit that Diatom must
not hold a core list - so the frontend is the wrong place to know it.

[ADR-0019](0019-input-mapping-and-remapping.md) rule 3 already says where that
kind of knowledge lives: the launcher owns configuration. The protocol simply
had no way to say it.

## Decision

**Display mode becomes a state on ADR-0020's plane, in the same shape as
everything else there.**

```
launcher → DISPLAY                              query
Diatom   → DISPLAY mode=integer filter=nearest rect=960x672+32+48
launcher → SETDISPLAY mode=aspect               filter is optional, and vice versa
```

Four things follow the plane's existing rules rather than inventing anything:

**Emitted from the one place the mode ever changes.** `apply_display` is the
single writer, so a launcher hears about the mode whether it asked, the user
cycled it with a chord, or the rect settled underneath it at boot (ADR-0021).
A second reporting path could disagree with the first, and would eventually.

**`rect=` rides along.** The launcher would otherwise have to recompute it from
geometry it does not have - and after ADR-0021 that geometry can change a second
into the session.

*Added 2026-10-02:* **and so does `surface=`**, the size the rect is measured in,
e.g. `rect=640x480+0+0 surface=640x480`. A launcher that draws at another size
scales the rect to its own; one that sees no `surface=` takes the rect as
already in its units. Found on the GKD Pixel 2, whose surface is 640x480 under
a launcher drawing at 1024x768: the paused frame behind the in-game menu sat in
the top-left 640x480 of the screen.

**All-or-nothing.** Both fields are validated before either is applied, for the
reason `SETMAP` is atomic: a half-applied setting is one nobody asked for and
neither side believes in.

**`RUN` resets to the process default**, which the existing code already does by
computing each session from `sn->mode`. Same argument as the remap table: a
resident frontend silently carrying the last game's mode into the next one
misfires precisely on the games nobody configured.

## Consequences

**This is the second contested row, and ADR-0020 set a threshold for that.**
Its "Revisit if" reads:

> A second state item turns out to be genuinely contested the way levels are.
> One such row is a decision; three would mean the ownership model is wrong.

Display mode is contested in exactly the same shape as volume and brightness:
Diatom owns it while a game runs, because the launcher is not drawing and the
chord is the only way to change it; the launcher owns it the rest of the time,
because it is the only side that can persist a preference. So the count is now
**two of three**. The model survives, and the next candidate should be read as
evidence against it rather than as another easy extension.

**Easier.** Per-system and per-game display modes become the launcher's business
without Diatom learning what a system is. ADR-0018's finding that the best mode
differs by system becomes actionable instead of merely recorded.

**Harder.** `proto=2` grows two more verbs, and a launcher that persists a mode
now has one more thing to store per game.

**A bug this exposed.** `g_mode` is a static and therefore starts at 0, which is
`diatom_modes[0]` - `integer`. The real default is `stretch`, and it was only
ever applied once a session started. That was harmless while nothing could ask.
Making the mode queryable meant an idle Diatom answered **`mode=integer` for a
frontend that would have used `stretch`**, and a launcher persisting that answer
would have written down a preference the user never expressed. Found by querying
with no game loaded, which is a state nothing had previously had a reason to
inspect.

**Verified on hardware against ADR-0021's settle**, which is the interaction
worth checking because both move the rect:

```
DISPLAY  mode=stretch  rect=1024x768+0+0     at load
geometry settled 256x192 -> 320x224 at frame 27
DISPLAY  mode=stretch  rect=1024x768+0+0     re-emitted; stretch fills either way
SETDISPLAY mode=integer -> rect=960x672+32+48
SETDISPLAY mode=aspect  -> rect=1003x768+10+0
```

The settle re-emits even when the rect does not move, because `apply_display` is
the single writer and does not know whether the caller will care.

**Not done.** Nothing chooses a mode automatically. Diatom still defaults to
`stretch` for everything, which after ADR-0021's measurement is the only mode
that was already correct on Genesis and is unchanged by it.

## Revisit if

- A third state turns out to be contested, which by ADR-0020's own threshold
  means the ownership split is the wrong model rather than a working one.
- A launcher wants the mode to survive `RUN` without re-sending it, which would
  mean the reset rule is wrong here even though it is right for the remap table.
