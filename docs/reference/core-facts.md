# Core facts - generated, do not edit

Regenerate with `tools/corefacts.sh`. Verify with `tools/corefacts.sh --check`.

**Every number here was measured, on the device, from a binary whose
sha256 matched [CORES.md](../../CORES.md) both before and after the push.**
Cite this file rather than restating its numbers - a figure copied into
prose cannot be re-checked, which is how ADR-0015 came to describe four
cores the project no longer runs.

Rows are per *content*, not per system: PAL and NTSC differ in both fps
and aspect, so one row per system cannot be correct.

| System | Core | sha256 | Base | Max | Aspect | fps | Rate |
|---|---|---|---|---|---|---|---|
| NES (PAL) | `fceumm` | `1b13b00d4680` | 256x224 | 256x240 | 1.3061 | 50.0070 | 48000 |
| NES (NTSC) | `fceumm` | `1b13b00d4680` | 256x224 | 256x240 | 1.3061 | 60.0998 | 48000 |
| SNES (PAL) | `snes9x2010` | `3933890f520a` | 256x224 | 1024x478 | 1.5842 | 50.0070 | 32040 |
| SNES (NTSC) | `snes9x2010` | `3933890f520a` | 256x224 | 1024x478 | 1.3061 | 60.0985 | 32040 |
| Game Boy | `mgba` | `abde7a0764f0` | 160x144 | 256x224 | 1.1111 | 59.7275 | 131072 |
| GBA | `mgba` | `abde7a0764f0` | 240x160 | 240x160 | 1.5000 | 59.7275 | 65536 |
| Genesis | `genesis_plus_gx` | `3673a22b9065` | 320x224 | 348x240 | 1.3061 | 59.9227 | 44100 |
| Master System | `genesis_plus_gx` | `3673a22b9065` | 256x192 | 284x240 | 1.5238 | 59.9227 | 44100 |
| Game Gear | `genesis_plus_gx` | `3673a22b9065` | 160x144 | 284x240 | 1.3333 | 59.9227 | 44100 |
| PC Engine | `mednafen_pce_fast` | `aca90a14b181` | 256x240 | 512x243 | 1.2150 | 59.8200 | 44100 |

## Pinned set at time of measurement

A change here invalidates every row above, which is the whole point:
`make check` compares this list against CORES.md without needing a
device, so swapping a core fails immediately rather than silently
aging every measurement that cited it.

- `fceumm` `1b13b00d4680394dad8000d5175f97be727107e0945bc9b412da91d70c07b267`
- `genesis_plus_gx` `3673a22b906509461e23a5a118b1d1bec15cbda105f260cbcbc08a16b2124e48`
- `mednafen_pce_fast` `aca90a14b18108c86398da2267ef40d5145eaddbc1c1b310614d745b258552b1`
- `mgba` `abde7a0764f08fa0cc2c7d3d9a29b9d1245a9f3b7df0e7a594b74df642ee53c6`
- `snes9x2010` `3933890f520abb9dbb0e5276460785b20ce54d25f552b369cafeca270b9dd44c`
