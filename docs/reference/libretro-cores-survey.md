# Libretro cores for the in-scope systems - survey

**Date:** 2026-08-23. A survey, not a recommendation. **For what was actually
chosen and why, see [core-selection.md](core-selection.md)** - this file is the
field, that one is the decision. Diatom has no core list
(see `working-agreement.md`); this exists so the **test matrix** is chosen from a known
field rather than from whatever happened to be on disk.

## Sources, and what each is worth

| Tag | Source | What it actually proves |
|---|---|---|
| `[org]` | The `libretro` GitHub organisation's repositories - 280 repos, 276 active, enumerated via `gh api` 2026-08-23 | That a repo of that name exists in that org. **Not** a registry: a libretro core is any shared library implementing the API, and it need not live there. |
| `[docs]` | [docs.libretro.com/guides/core-list](https://docs.libretro.com/guides/core-list/) | That libretro documents it. A curated view, and curation lags. |
| `[measured]` | The [env-inventory spike](../spikes/2026-08-23-env-inventory.md), run here | A fact about a specific binary I ran. |
| `[community]` | Forums, guides, wikis - cited inline | **Opinion.** Widely-held opinion is still opinion. |
| `[unverified]` | - | No evidence for Cortex-A53 / 1 GB. Must be measured on the Brick. |

**Neither catalogue is complete, and they disagree.** `Geargrafx`, `Gearsystem`
and `ClownMDEmu` appear in `[docs]` but are **not** repos in the libretro org -
they are their authors' own projects. Treating either source as authoritative
would miss cores.

---

## NES

| Core | Sources | Character |
|---|---|---|
| **Mesen** | `[org]` `[docs]` | Most accurate `[community]`; heaviest |
| **Nestopia (UE)** | `[org]` `[docs]` | Accurate, moderate cost `[community]` |
| **FCEUmm** | `[org]` `[docs]` `[measured]` | Fast, broad compatibility. Measured: 48000 Hz, requests `GET_CURRENT_SOFTWARE_FRAMEBUFFER` every frame |
| **QuickNES** | `[org]` `[docs]` | Fastest, lowest accuracy `[community]` |
| **fixNES** | `[org]` `[docs]` | - |
| **bnes** | `[org]` `[docs]` | Older |

> "Mesen is the most accurate NES core… on a low-power device like a Raspberry Pi 3, FCEUmm is lighter" - `[community]`

## Master System / Game Gear

| Core | Sources | Character |
|---|---|---|
| **Genesis Plus GX** | `[org]` `[docs]` | Covers SMS, GG, SG-1000, Genesis, Sega CD. Accurate `[community]` |
| **SMS Plus GX** | `[org]` `[docs]` | SMS/GG only, lighter |
| **Gearsystem** | `[docs]` **not `[org]`** | drhelius's own repo |
| **PicoDrive** | `[org]` `[docs]` `[measured]` | Advertises `sms\|gg\|sg` - covers these too `[measured]` |

## PC Engine / TurboGrafx-16 (+ CD)

| Core | Sources | Character |
|---|---|---|
| **Beetle PCE** | `[org]` `[docs]` | Full accuracy variant; also SuperGrafx |
| **Beetle PCE FAST** | `[org]` `[docs]` `[measured]` | Performance variant. Measured: 59.82 fps, 44100 Hz, `max 512×243`, HuCard + CD |
| **Beetle SuperGrafx** | `[org]` `[docs]` | SGX-specific |
| **Geargrafx** | `[docs]` **not `[org]`** | drhelius; newer |

## Genesis / Mega Drive

| Core | Sources | Character |
|---|---|---|
| **Genesis Plus GX** | `[org]` `[docs]` | The accuracy default `[community]`; `-Wide` variant also in org |
| **PicoDrive** | `[org]` `[docs]` `[measured]` | "written having ARM-based handheld devices in mind" `[community]`. Adds 32X + Sega CD. Measured: 60.0000 fps exactly - the only in-scope core that does |
| **BlastEm** | `[org]` `[docs]` | High accuracy, heavier `[community]` |
| **ClownMDEmu** | `[docs]` **not `[org]`** | Clownacy's own repo |

## SNES - the widest field, and the one that matters most

Accuracy/performance spread is larger here than anywhere else, and SNES is the
system whose coprocessors (SuperFX, SA-1, DSP-1, CX4) are the known pinch point
on A53-class hardware `[unverified for the Brick]`.

| Core | Sources | Tier `[community]` |
|---|---|---|
| **bsnes** · **bsnes-jg** · **bsnes-hd** | `[org]` `[docs]` | Accuracy, heavy |
| **bsnes-mercury** Accuracy/Balanced/Performance | `[org]` `[docs]` | Three explicit tiers |
| **bsnes 2014** Accuracy/Balanced/Performance | `[org]` `[docs]` | Older bsnes, three tiers |
| **Mesen-S** · **nSide Balanced** · **Beetle bsnes** | `[org]` `[docs]` | Accuracy |
| **Snes9x** (1.63) | `[org]` `[docs]` `[measured]` | Balanced default. Measured: 32040 Hz, `max 604×478`, `SET_GEOMETRY` mid-run |
| **Snes9x 2010** | `[org]` `[docs]` | "serving more games in more low powered devices like a Raspberry Pi" `[community]` |
| **Snes9x 2005** · **2005 Plus** | `[org]` `[docs]` | Lighter again `[community]` |
| **Snes9x 2002** | `[org]` `[docs]` | Lightest, least accurate |
| **Beetle Supafaust** | `[org]` `[docs]` | Explicitly targeted at "low-end devices, such as multicore ARM Cortex A7, A9, A15, **A53** Linux platforms" `[community]` - the only core whose own description names our exact CPU |
| **ChimeraSNES** | neither - `jamsilva/chimerasnes` | Fork aimed at low-end |

## Game Boy / Game Boy Color

| Core | Sources | Character |
|---|---|---|
| **SameBoy** | `[org]` `[docs]` | Most accurate `[community]`, heavier |
| **Gambatte** | `[org]` `[docs]` `[measured]` | The common default. Measured: 59.7275 fps, 32768 Hz |
| **Gearboy** | `[org]` `[docs]` | Lighter `[community]` |
| **TGB Dual** | `[org]` `[docs]` | Link-cable / dual |
| **Emux GB** | `[org]` `[docs]` | Obscure |
| **mGBA** | `[org]` `[docs]` `[measured]` | Advertises `gb\|gbc\|sgb` - covers these too `[measured]` |

## Game Boy Advance

| Core | Sources | Character |
|---|---|---|
| **mGBA** | `[org]` `[docs]` `[measured]` | Accurate, actively developed `[community]`. Measured: 59.7275 fps, **65536 Hz** |
| **gpSP** | `[org]` `[docs]` | Dynarec, aimed at weak ARM `[community]`; some title-specific issues |
| **VBA Next** · **VBA-M** | `[org]` `[docs]` | Older lineage |
| **Beetle GBA** | `[org]` `[docs]` | VBA-derived |
| **TempGBA** · **Meteor** | `[org]` `[docs]` | Older / niche |

---

## What this changes for Diatom

**Nothing structural** - Diatom loads whatever core it is handed. Two practical
consequences:

1. **The test matrix should span the accuracy/performance spread, not sit in one
   band.** The six cores measured so far are all mid-tier. Verifying against a
   heavy core (`bsnes-mercury Accuracy`, `Mesen`) *and* a light one
   (`Snes9x 2002`, `Supafaust`) would prove nothing in Diatom is tuned to one
   band's behaviour. That is register §12's open item.
2. **Multi-system cores shrink the matrix.** `[measured]` PicoDrive covers
   Sega 8- and 16-bit; mGBA covers GB/GBC and GBA; Beetle PCE Fast covers both
   PC Engine media. Genesis Plus GX reportedly covers five Sega systems.

**Every performance claim above is `[community]` or `[unverified]`.** Nothing
here has been measured on a Cortex-A53. Beetle Supafaust naming A53 in its own
description is the strongest signal available, and it is still a claim, not a
measurement.
