# 2026-08-25 - core options

Cores declare what they can be configured with and query each value on demand.
Diatom accepted the declarations, discarded them, and answered every query with
"not set" - so every core ran on built-in defaults and nothing was reachable.

## What was built

`src/options.c`, plus real answers in `env.c` for `SET_CORE_OPTIONS_V2_INTL`,
`SET_CORE_OPTIONS_V2`, `SET_VARIABLES`, `GET_VARIABLE` and
`GET_VARIABLE_UPDATE`.

**Ownership follows the same split as everything else:** Diatom holds the
definitions and the current values; the **launcher** decides what those values
should be. Diatom has no opinion about whether a Genesis should be PAL and no
way to ask, having no UI (ADR-0009). Reachable today by `--core-option
key=value`; the protocol surface comes with the launcher menu.

**v1 (`SET_CORE_OPTIONS`) is accepted and not parsed.** No core in the matrix
uses it, and untested code that silently mis-parses options is worse than a core
falling back to its own defaults. Same treatment ADR-0007 gave XRGB8888.

**String lifetime rests on ADR-0006.** Keys and values are copied; `desc` and
the value list are borrowed, which is only safe because cores are never
unloaded. Written down in the source, because it would dangle the day that
changes.

## What the cores actually offer

| Core | Options |
|---|---|
| `genesis_plus_gx` | **62** |
| FCEUmm | **44** |
| `mednafen_pce_fast` | 32 |
| mGBA | 17 |
| `snes9x2010` | 16 |

**171 settings across five cores, all of them previously unreachable.**

## Finding: option availability can depend on the loaded content

**FCEUmm declares zero options at core open and 44 once a ROM is loaded.** The
other four declare everything at open.

That is not a curiosity, it is a constraint on the launcher menu: **a core's
option list cannot always be shown from the shelf.** For FCEUmm the menu is only
complete once the game is running, which fits the in-game menu ADR-0016 already
requires and rules out an options screen that browses cores without loading
anything.

`--list-options` therefore lists after loading when a ROM is given and before
when it is not; listing what a core offers should not *require* owning a game.

## Verified by observable effect, not by absence of errors

```
region default (Auto)   256x224  aspect 1.3061  60.0998 fps
region=PAL              256x224  aspect 1.3061  50.0070 fps
overscan default (8/8)  256x224  aspect 1.3061
overscan 0/0            256x240  aspect 1.2190
overscan 24/24          256x192  aspect 1.5238
```

Forcing PAL changes the rate the core reports and Diatom repaces to it. Overscan
cropping changes both geometry and the aspect the core asks for, because the
core compensates. A value the core does not offer is refused with a warning and
the default kept, rather than handed over - a core given a value outside its own
list is entitled to do anything at all.

## Consequence: a core swap silently changed NES geometry

The buildbot FCEUmm crops **8 lines of overscan by default**, reporting
**256x224, aspect 1.3061**. The convenience-sample build used for the display
mode work reported **256x240, aspect 1.2190**.

So the NES row of the tables in ADR-0015 and the display spikes describes a
binary we no longer use. **The decisions stand** - `stretch` was chosen on the
panel, and stretch fills it whatever the source geometry - but the integer and
aspect rects for NES would now differ.

This is the drift `CORES.md` exists to catch, and it appeared within a day of
that file being written. It also means overscan is now a *choice*: `--core-option
fceumm_overscan_v_top=0` restores the full 240 lines.
