# Spike result - environment-call inventory

- **Date:** 2026-08-23
- **Question:** *Which `RETRO_ENVIRONMENT_*` calls do our cores actually make, at
  which lifecycle phase, and which must Diatom implement?*
- **Status:** Answered.
- **Harness:** [`tools/envlog.c`](../../tools/envlog.c), built with `make tools`.
  Tracked as an *instrument* rather than left as a throwaway spike, so these
  numbers can be re-checked. Not Diatom code and never linked into it.

## Method

A ~200-line throwaway harness `dlopen`s a core, installs an environment callback
that logs every command with its lifecycle phase, answers a minimal set, and
declines the rest - a decline is still a data point. Then `retro_init`,
`retro_load_game` with a real ROM, 120 × `retro_run`, `retro_unload_game`,
`retro_deinit`.

Command names are generated from `libretro.h` itself, so they cannot drift.

Run inside an aarch64 Linux container, so aarch64 cores load with **no device
involved**.

All six cores, real ROMs, full lifecycle.

### On the cores tested

The six were a **convenience sample** - aarch64 binaries already on disk - not a
core list and not a recommendation. Diatom has no core list; it loads whatever it
is handed (see `../working-agreement.md`). They were chosen because they cover the systems in
scope and were to hand.

libretro cores are independent upstream binaries, buildable from `libretro-super`
by anyone. Substituting `genesis_plus_gx` for `picodrive`, or a lighter `snes9x`
fork, changes nothing structural - it means re-running this harness, which takes
minutes.

**What generalizes** beyond these builds: the environment-call inventory (an
API-usage pattern shared across libretro cores), and the frame and sample rates
(properties of the emulated hardware). **What does not**: exact option counts, and
any core-specific quirk.

## Per-core facts

| Core | System | fps | Sample rate | Base | Max | Pixfmt | Options |
|---|---|---|---|---|---|---|---|
| FCEUmm | NES (PAL) | **50.0070** | 48000 | 256×240 | 256×240 | RGB565 | 33 |
| Snes9x 1.63 | SNES (PAL) | **50.0070** | **32040** | 256×224 | 604×478 | RGB565 | 40 |
| Gambatte | GB | 59.7275 | 32768 | 160×144 | 160×144 | RGB565 | 17 |
| PicoDrive | Genesis | 60.0000 | 44100 | 320×240 | 320×240 | RGB565 | 21 |
| Beetle PCE Fast | PC Engine | 59.8200 | 44100 | 256×243 | 512×243 | RGB565 | 31 |
| mGBA 0.11-dev | GBA | 59.7275 | **65536** | 240×160 | 240×160 | RGB565 | 17 |

## The headline numbers

**34 of the environment commands appear. About 17 must be implemented. 17 are
declined by every core with nothing breaking. 43 never appear at all.**

The long tail turned out to be a checklist.

### Universal - all six cores

`GET_SYSTEM_DIRECTORY` · `SET_PIXEL_FORMAT` · `SET_INPUT_DESCRIPTORS` ·
`GET_VARIABLE` · `GET_VARIABLE_UPDATE` · `GET_LOG_INTERFACE` · `GET_LANGUAGE` ·
`GET_CORE_OPTIONS_VERSION` · `SET_CORE_OPTIONS_V2_INTL` ·
`GET_INPUT_BITMASKS` *(declined by all six, nothing broke)*

### Must implement

| Cmd | Call | Cores | Note |
|---|---|---|---|
| 9 · 31 | `GET_SYSTEM_DIRECTORY` · `GET_SAVE_DIRECTORY` | 6/6 · 1/6 | BIOS and saves |
| 10 | `SET_PIXEL_FORMAT` | 6/6 | **all six chose RGB565** |
| 15 | `GET_VARIABLE` | 6/6 | up to **38 calls** at load |
| 17 | `GET_VARIABLE_UPDATE` | 6/6 | **every frame** |
| 37 | `SET_GEOMETRY` | 3/6 | **fires during `run`** |
| 47 | `GET_AUDIO_VIDEO_ENABLE` | 1/6 | **every frame** (snes9x) |
| 52 · 68 | `GET_CORE_OPTIONS_VERSION` · `SET_CORE_OPTIONS_V2_INTL` | 6/6 | the only options path any core uses |
| 3 · 8 · 11 · 27 · 35 · 36 · 39 · 42 | can-dupe, perf level, input descriptors, log, controller info, memory maps, language, achievements | 1-6/6 | accept, mostly ignore |

### Safely declined by every core that asked

`SET_DISK_CONTROL_INTERFACE` · `GET_DISK_CONTROL_INTERFACE_VERSION` ·
`GET_RUMBLE_INTERFACE` · `GET_PERF_INTERFACE` · `SET_SUBSYSTEM_INFO` ·
`GET_CURRENT_SOFTWARE_FRAMEBUFFER` · `GET_VFS_INTERFACE` · `GET_INPUT_BITMASKS` ·
`SET_CORE_OPTIONS_DISPLAY` · `GET_MESSAGE_INTERFACE_VERSION` ·
`SET_AUDIO_BUFFER_STATUS_CALLBACK` · `SET_MINIMUM_AUDIO_LATENCY` ·
`SET_FASTFORWARDING_OVERRIDE` · `SET_CONTENT_INFO_OVERRIDE` · `GET_GAME_INFO_EXT` ·
`SET_VARIABLE` · `GET_TARGET_SAMPLE_RATE`

## Findings that change the design

### 1. No console runs at 60 Hz, and five of six are not even close

Five distinct frame rates across six cores: **50.0070, 59.7275, 59.8200,
60.0000**. Only PicoDrive hits exactly 60. Both PAL ROMs report 50.0070, and PAL
is a deliberate target - Probotector is PAL-only Contra.

**Pacing 50 Hz content on a 60 Hz panel is the normal case, not an edge case.**
Dynamic rate control moves from "the mature approach" to mandatory. This makes
§6/§7 pacing the most interesting open problem in the project.

### 2. Sample rates span 32040 - 65536 Hz

Five distinct values. mGBA outputs at **65536 Hz**, above any device rate, so
resampling happens in both directions and never with a tidy ratio. Confirms
ADR-0007's decision to have the host resample to the port's reported rate.

### 3. Two pacing levers we did not know existed

- **`GET_TARGET_SAMPLE_RATE`** (81) - one core *asks what rate we want*. Answering
  it lets that core generate audio at the device rate natively, skipping
  resampling entirely for that core. Cheap win.
- **`SET_AUDIO_BUFFER_STATUS_CALLBACK`** (62) - three of six offer to *receive*
  buffer occupancy, so the core can throttle itself. That is the core-side half
  of dynamic rate control. Declining works; implementing it is a second lever.

Neither was on the radar before this spike.

### 4. The XRGB8888 path is currently dead code

**All six cores chose RGB565.** ADR-0007 accepts both formats, which costs
nothing and remains right - but the 8888 path should not be built or tested
until something actually needs it.

### 5. `SET_GEOMETRY` during `run` is common, not exotic

Three of six - snes9x, picodrive, Beetle PCE. Confirms ADR-0007's requirement to
recompute `dst` **on change** rather than at load. Snes9x reports `max 604×478`
against `base 256×224`; Beetle PCE reports `max 512×243`.

### 6. PC Engine breaks integer scale on the Miniloong but not the Brick

Beetle PCE's 256×**243** at 3× is 768×**729**. That exceeds the Miniloong's
720-line surface, so PCE gets 2× (512×486) there and 3× on the Brick's 768 lines.

**The same system scales differently per device** - a live instance of ADR-0007's
"when integer scale doesn't fit" default, appearing on the very first real
measurement.

### 7. `GET_CURRENT_SOFTWARE_FRAMEBUFFER` is actively offered

FCEUmm requests it **every frame**. The zero-copy path ADR-0007 deferred is
available whenever it is wanted. Still right to defer; good to know it is real.

### 8. One core often covers several systems

Reported `valid_extensions`, useful because it means the test matrix needs fewer
cores than it has systems:

- **PicoDrive** - `bin|gen|smd|md|32x|cue|iso|chd|sms|gg|sg|sc|m3u|...`: Genesis,
  Master System, Game Gear, 32X, Sega CD.
- **mGBA** - `gba|gb|gbc|sgb`: GBA and Game Boy both.
- **Beetle PCE Fast** - `pce|cue|ccd|chd|toc|m3u`: HuCard and CD.

This is an observation about these binaries, not a recommendation. A host might
reasonably prefer `genesis_plus_gx` for Sega or `gambatte` for Game Boy on
accuracy grounds - Diatom does not care either way.

## Harness bugs worth remembering

Both produced plausible-looking wrong answers before being caught:

1. **Experimental commands have `0x10000` baked into their `#define`.** Masking
   the incoming command but not the `case` labels made every experimental
   command fall through to "declined". Mask both sides.
2. **Cores use `SET_CORE_OPTIONS_V2_INTL`, not `SET_CORE_OPTIONS_V2`.** Handling
   only the non-INTL variant reported *zero* core options for every core - a
   believable-looking result that was entirely an artifact.

## Correction, 2026-08-24

Promoting the harness to `tools/` surfaced a bug in it. The command-name table
was keyed on the low 16 bits, but **two commands share number 44** and are told
apart only by the experimental bit:

```
#define RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS 44
#define RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT   (44 | RETRO_ENVIRONMENT_EXPERIMENTAL)
```

A masked table can hold only one of them and silently mislabels the other; which
one was lost depended on iteration order. The generator now keys on the full
value, giving 78 entries rather than 77.

**No result above is affected** - command 44 was never observed in any run. But
it would have mislabeled any core that used either command, and the throwaway
version would never have been looked at again.
