# Spike result - RSS with all cores resident, and `dlopen` cost

- **Date:** 2026-08-23
- **Question:** *What does holding every core resident actually cost in RSS, and
  what does `dlopen` cost on this CPU?*
- **Purpose:** evaluate [ADR-0006](../decisions/0006-keep-all-cores-resident.md)'s
  revisit trigger - *"measured RSS with all cores mapped and one initialized
  exceeds ~250 MB"* - which until now was backed by an estimate.
- **Status:** Answered. **Trigger not met, by a factor of ~16.**
- **Harness:** [`tools/rssprobe.c`](../../tools/rssprobe.c), built with `make tools`.
  ../working-agreement.md practice 7.

## Hardware - measured, not assumed

TrimUI Brick over USB ADB:

```
CPU part 0xd03  →  Cortex-A53        MemTotal    998,332 kB  ≈ 975 MB
kernel 4.9.191                        SwapTotal         0 kB
```

**Zero swap is now verified**, not assumed. That is the fact underneath the
whole file-backed-versus-anonymous argument in ADR-0006 and ADR-0010.

## Result

Six cores `dlopen`'d `RTLD_NOW | RTLD_LOCAL` (ADR-0010), one initialized, one
game loaded, 720 frames run.

```
baseline (process only)             RSS     216 kB
dlopen fceumm                       RSS    3076 kB  (+ 2860)
dlopen gambatte                     RSS    5408 kB  (+ 5192)
dlopen snes9x                       RSS    6276 kB  (+ 6060)
dlopen picodrive                    RSS    6972 kB  (+ 6756)
dlopen mednafen_pce_fast            RSS   10808 kB  (+10592)
dlopen mgba                         RSS   13108 kB  (+12892)
--- 6 cores mapped ---
retro_init (1 core)                 RSS   13108 kB  (+12892)    0.0 ms
retro_load_game                     RSS   15140 kB  (+14924)    6.2 ms
120 x retro_run                     RSS   15404 kB  (+15188)  217.6 ms
+600 x retro_run                    RSS   15404 kB  (+15188) 1087.4 ms
```

### **6 cores mapped + 1 running = 15.0 MB**

Against a 250 MB trigger and 975 MB of RAM. **1.5% of the device's memory.**

## The estimate was wrong by 10×

ADR-0006 reasoned from *"peak loaded game roughly 10-30 MB… ~150 MB with five to
seven cores resident."* Actual: **15 MB**.

Where the estimate went wrong: a mapped `.so` costs far less RSS than its file
size, because only the pages actually touched become resident. `mednafen_pce_fast`
is 4.3 MB on disk and adds 3.6 MB; `picodrive` is 1.4 MB on disk and adds 0.8 MB.
Roughly **2 MB resident per mapped core**, and the loaded game adds only ~2 MB
more.

The estimate was wrong in the safe direction and the decision was never close -
but it was a guess presented with a number attached, and this is what it is worth.

**The trigger is now badly calibrated.** Set at 250 MB against an estimate of
150 MB, it would take a 16× regression to fire. As an early warning it is useless.
A trigger around 50 MB would actually mean something. Recorded here rather than
by superseding ADR-0006, since the decision itself is confirmed.

## `dlopen` cost - cache state dominates

| Core | Cold (caches dropped) | Warm |
|---|---|---|
| fceumm | 42.9 ms | 4.3 ms |
| gambatte | 41.0 ms | 9.7 ms |
| snes9x | 24.3 ms | 6.1 ms |
| picodrive | 8.3 ms | 2.1 ms |
| mednafen_pce_fast | 70.1 ms | 9.1 ms |
| mgba | 45.1 ms | 4.2 ms |
| **all six** | **232 ms** | **35 ms** |

RSS is effectively identical in both cases (12.9 vs 13.1 MB) - page-cache state
changes *time*, not resident memory.

**This does not reproduce PlayOS's `~170 ms` per-core figure.** Worst cold case
here is 70 ms; warm is single digits. The two measurements differ in context -
PlayOS's number may cover more than the `dlopen` call, or have been taken under
different card and cache conditions - and I cannot say which from here. Recorded
as a discrepancy, not as a correction to that comment.

The practical consequence stands either way: **mapping all six cores once costs
~232 ms cold, and never recurs**, because ADR-0006 never unloads.

## Execution headroom

FCEUmm, NES, PAL:

- 120 frames in 217.6 ms and 600 frames in 1087.4 ms - **1.81 ms/frame**, dead
  steady across both.
- PAL budget at 50.0070 Hz is **20.0 ms/frame**.
- So core execution uses **~9% of frame budget** on a Cortex-A53.

Caveat: this is core execution only. No scaling, no blit, no audio resampling,
no presentation. The remaining 91% is what Diatom has to fit into, not free
headroom.

## No leak observed

RSS was **identical at 15,404 kB after 120 frames and after 720** - not a
kilobyte of growth. One core, one game, one session, so this is not proof that
cores never leak. It is evidence that this one does not, over that span.

## What this settles

- **ADR-0006 confirmed with a real number**, not an estimate. Residency was never
  close to a memory problem.
- **ADR-0010's premise verified** - zero swap, so anonymous memory is
  unreclaimable, and the file-backed/anonymous distinction is real on this device.
- **The Brick's row in the device table** (§0b) is no longer blank: 975 MB,
  Cortex-A53, no swap.
