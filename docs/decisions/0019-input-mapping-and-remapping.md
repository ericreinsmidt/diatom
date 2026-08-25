# 0019. Two input translations, and only one of them is remappable

- **Status:** Accepted
- **Date:** 2026-08-25
- **Supersedes:** -
- **Superseded by:** -

Extends [ADR-0007](0007-port-interface.md) and [ADR-0003](0003-digital-only-input.md),
both unchanged.

## Context

A user of another firmware wanted to remap buttons and found it very difficult.
That is worth taking seriously as a design signal, because the difficulty is
almost never in the remapping itself - it is in *where the mapping was put*.

Diatom already has two translations between a finger and a core:

    physical event  ->  canonical Diatom button  ->  retropad id  ->  core
        (port)                              (env.c)
    BTN_EAST              DIATOM_BTN_A       RETRO_DEVICE_ID_JOYPAD_A

**These are different kinds of thing and collapsing them is the mistake.**

The first is a **device fact**. On the Brick the cap labelled A emits
`BTN_EAST`, and X and Y are physically the opposite way round from their
positions - the top cap emits `BTN_WEST`, the left cap `BTN_NORTH`. That was
measured twice under `DIATOM_INPUT_DEBUG` precisely because positional
reasoning got those two wrong. It is not a preference and no user should be
able to edit it.

The second is **policy**: "I want X to act as B."

A firmware that goes straight from `BTN_EAST` to retropad A in one hardcoded
step has nowhere to put a remap. Adding one means editing device code, every
backend reimplements it, and the implementations drift. That is the shape of
the problem the user hit.

The register asked *"Per-core remapping, or one map?"* and it was closed on
2026-08-25 with **"One map. No core in the matrix has asked for anything else,
and a remapping layer is an abstraction with no consumer until one does."**
That reasoning was wrong twice over, and both are worth recording:

- It invoked §0's seam test, which is about **interfaces with implementations
  behind them**. A remap table is configuration, not a seam. The test did not
  apply; it was the nearest available justification.
- It asked whether any **core** wanted remapping. Cores do not want remapping;
  people do. For an internal abstraction "no consumer yet" is sound. For a
  user-facing capability it is circular, because the consumer does not have the
  software yet and cannot file a request against it.

## Decision

**Four rules, and the value is in the boundaries between them.**

1. **The port owns physical → canonical, and it is never configurable.** It
   encodes measured hardware truth. Device quirks resolve there and never
   travel upward - which is already how the Brick's front keys reporting as
   `BTN_THUMBL`/`THUMBR` is handled.

2. **The host owns canonical → retropad, and it is DATA, not code.** Today
   `button_map[]` in `env.c` is `static const`. It becomes a mutable table
   defaulting to identity. This is the only layer a remap touches, so there is
   exactly one implementation of remapping regardless of how many ports exist.

3. **The launcher owns the configuration and the UI**, delivered over the
   ADR-0009 socket in the same shape as `SETOPT`. Whether a map is global,
   per-system or per-game is the launcher's business; Diatom receives a table
   and does not care where it came from.

4. **MENU is unmappable, by construction rather than by policy.** Remapping
   changes how `cb_input_state` *answers* a query; it never rewrites the input
   state the frame loop reads. So MENU, display chords and any future hotkey
   see unmapped physical state and cannot be remapped away.

That fourth rule is the one that needs stating, because the tempting
implementation - rewriting the button bits as they are read - looks equivalent
and is not. It would let a user map away the button that opens the menu, with
no way to reach the screen that would undo it.

## Consequences

**Easier.** Remapping is one table in one file. A new port inherits it for
free and cannot get it wrong, because it never sees it.

**Harder.** The launcher now needs a remap UI to make this reachable, and a
protocol message to carry it. Diatom is useless for remapping on its own -
which is correct, since UI belongs to the launcher (ADR-0009), but it does mean
this decision delivers nothing visible until a launcher uses it.

**A gap this exposes.** `SET_INPUT_DESCRIPTORS` is currently in `env.c`'s
"accepted and ignored" list. That is the core telling us *"B = Jump, A = Fire"*
for this specific game, and it is exactly what a remap screen needs to be
usable: without it the UI reads `B → ?`, with it `Jump → ?`. It arrives free on
every load and we discard it. Capturing it is not part of this decision but is
now tracked in §8.

**Not foreclosed.** Per-system and per-game maps need no change here - they are
different tables from the launcher. Analog stays out entirely (ADR-0003).

## Revisit if

- A core turns out to need a different canonical set rather than a different
  mapping of the existing one, which would mean the canonical buttons
  themselves are wrong rather than their assignment.
- A device appears whose physical layout cannot be expressed as
  `DIATOM_BTN_*`, which would push work back into rule 1 and is the case this
  ADR is least confident about.
