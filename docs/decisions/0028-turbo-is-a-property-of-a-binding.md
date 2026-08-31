# 0028. Turbo is a property of a binding, and the host owns it

- **Status:** Proposed
- **Date:** 2026-08-31
- **Supersedes:** -
- **Superseded by:** -

Extends [ADR-0019](0019-input-mapping-and-remapping.md) and
[ADR-0020](0020-shared-state-plane.md), both unchanged.

## Context

Seven of the nine systems TortOS ships have **two face buttons** on the real
hardware: NES, Master System, Game Boy, TurboGrafx-16, Game Gear, Game Boy Color
and Game Boy Advance. Genesis has three, or six with the pad Genesis Plus GX
advertises as `MD Joypad 6 Button`. SNES has four. The Brick has four, so on
those seven X and Y are spare.

They are spare the whole way down. `button_map` sends canonical X and Y to
`RETRO_DEVICE_ID_JOYPAD_X` and `_Y` (`src/env.c:279`), nothing else in Diatom
claims them, and a two-button core ignores those ids. Pressing them today does
nothing at all.

**Turbo through core options reaches two of the seven.** Measured 2026-08-31 by
dumping every option key each core on the card declares:

| core | systems | turbo option |
|---|---|---|
| `fceumm` | NES | `fceumm_turbo_enable`, `fceumm_turbo_delay` |
| `mednafen_pce_fast` | TurboGrafx-16 | `pce_fast_turbo_toggling`, `_delay`, `_toggle_hotkey` |
| `genesis_plus_gx` | Master System, Genesis, Game Gear | **none**, of 64 declared |
| `mgba` | Game Boy, GBC, GBA | **none**, of 17 declared |
| `snes9x2010` | SNES | **none** |

And the two that have it disagree about what it is. FCEUmm publishes two
*dedicated turbo buttons* on `JOYPAD_X` and `_Y`, which is why NES turbo cost
TortOS one config line and no code (TortOS `a338f7c`). Mednafen's makes buttons
III and IV into *hotkeys that toggle* turbo for I and II - a mode, not a button.
Its L3/R3 variant is unreachable here regardless: the Brick's front keys report
`BTN_THUMBL`/`BTN_THUMBR` and the port leaves them unmapped by construction
(register section 8).

So the config-only route gives five systems nothing and gives the other two
different interactions from each other.

Two constraints bound any answer. ADR-0019 rule 1: the port owns
physical-to-canonical and **is never configurable**. ADR-0020: the launcher owns
configuration, Diatom persists none of it, and retropad ids stay out of the
protocol exactly as `libretro.h` stays out of the port (ADR-0007).

*Assumed, not measured:* the face-button counts are ordinary hardware facts, not
something checked on the device. A wrong row changes that row's case, not the
argument.

## Options considered

### Option A - core options only, as today

Zero code, zero protocol surface, and already working on NES. Honestly the
strongest option if the answer is "turbo is a per-core nicety, not a feature of
the frontend".

It costs five of the seven systems entirely, leaves the two it serves behaving
differently from each other, and makes the feature's existence depend on which
core author happened to implement it. It also cannot be uniform: no amount of
configuration turns Mednafen's toggle into FCEUmm's held button.

### Option B - turbo in the port

The port already sees the physical press and could pulse the canonical bit.

Rejected: it requires the port to be configurable, which ADR-0019 rule 1 forbids
in as many words, and the rule is load-bearing rather than tidy. It would also
have to be built once per port, and the desktop port would need it to test.

### Option C - a separate `TURBO` / `SETTURBO` state item

Follows ADR-0020's shape exactly and keeps `SETMAP` meaning one thing.

Rejected because it creates **two tables that both answer "what does X do"**,
and nothing makes them agree. `SETMAP map=x:none` with `SETTURBO turbo=x:a:3`
has no defined winner, and a launcher can produce that pair without doing
anything obviously wrong. ADR-0020 exists to stop exactly this class of silent
disagreement; adding one to implement it would be a poor joke.

### Option D - turbo as a property of a map binding *(chosen)*

A turbo binding **is** a binding. `x` acting as a pulsed `a` is the same kind of
statement as `x` acting as `a`, so it belongs in the same field, applied whole,
rejected whole, reset by `RUN`, with every one of those properties inherited
from ADR-0020 rather than restated.

## Decision

**Extend the map's value grammar with a pulse, and implement it in the host.**

```
launcher -> SETMAP map=x:a~3,y:b~3
Diatom   -> MAP map=x:a~3,y:b~3
```

`<source>:<target>~<frames>` means *while source is held, report target pressed
for `<frames>` frames, then released for `<frames>` frames, repeating.* Without
`~` a binding is unchanged. `<frames>` is an integer 1..30.

Five rules, each because the alternative is a bug:

**Frames, not milliseconds.** The core advances in frames. A frame-counted pulse
is deterministic, reproducible in a test, and does not drift when the frame rate
does. At 60 Hz `~3` is ten presses a second.

**The pulse is computed from the host's frame number, not per call.**
`cb_input_state` is called once per queried id, several times a frame. Deriving
the phase per call would let two buttons in the same frame disagree about where
in the cycle they are, which is a bug that would present as "turbo sometimes
eats a shot" and would be miserable to find.

**Phase resets on the source's release-to-press edge**, so the first frame of a
press is always ON. Anchoring to an absolute frame counter instead would start a
press on an OFF half-cycle half the time, and the button would feel like it
missed.

**The pulse applies per source button, before the OR.** `cb_input_state`
already ORs across every canonical button mapping to an id (ADR-0020). Holding A
and turbo-X together must give a continuously held A, not an interference
pattern between them.

**`none` and `menu` remain illegal targets, and `~0` is rejected.** The first two
are ADR-0019's rules, already enforced in `diatom_input_set_map`. `~0` is a
plain binding wearing a costume.

**Version: `READY proto=3`.** This changes the *value grammar* of an existing
key, which ADR-0009's ignore-unknown-keys promise does not cover. Verified in the
parser rather than assumed: a Diatom that predates this calls
`button_by_name("a~3")`, gets -1, and returns false, so the map is rejected whole
and answered with the unchanged `MAP` per ADR-0020. A new launcher against an old
Diatom therefore degrades to *no turbo*, never to a wrong map. `proto=` is how it
knows without probing.

The port is untouched, on every platform including desktop.

## Consequences

**Easier.** All seven two-button systems get turbo, with one interaction, from
one implementation. It is expressed canonically, so it says "X turbos A" and
means the same sentence on a Game Boy and on a NES. The launcher can ship it as
a per-system default in the shape it already uses for display mode and core
options, with no remap screen; a screen later configures the same field.

**Harder.** The map grammar grows a second concept, so both parsers change and
`proto` moves to 3 - a real compatibility surface, and the second time ADR-0020's
version story has been spent. The host gains per-button pulse state, which is
mutable per-frame state in a file that currently has almost none. And turbo is
genuinely awkward to verify by looking: today's session established that
confirming a *rate* on hardware is hard. The honest test is `test/stubcore.c`
counting `JOYPAD_A` transitions over a known number of frames with X held, which
should be written before the feature, not after.

**Foreclosed.** Nothing, but two things get pointedly not done: separate on and
off durations (`~3:5`), and a toggle-style turbo where a press latches auto-fire
until pressed again. The grammar has room for the first; the second is a
different feature and would want its own record.

**A tidy-up this implies.** With `x:a~3` sent for NES, X no longer reports
`JOYPAD_X` at all, so FCEUmm's own Turbo A can never fire and
`fceumm_turbo_enable=Player 1` in TortOS's `coreopts.cfg` becomes dead config.
It should be removed when this lands, not left as a second way to get the same
feature on one system out of nine.

## Revisit if

- A launcher wants the rate to differ **per game** rather than per system, which
  the current field can express but nothing stores.
- Someone wants turbo on a button that is not spare - Genesis or SNES - which a
  held binding cannot give them and a toggle could. That is Option C's problem
  arriving by another door, and it would need a real answer rather than an extra
  character of grammar.
- A core turns up whose own turbo is better than this one, for instance because
  it knows a game's timing. Then the question is whether both may exist, and the
  tidy-up above becomes a decision instead of housekeeping.
