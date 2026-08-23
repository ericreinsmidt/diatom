# 2026-08-23 — Scoping closed

Second and final session of the scoping phase. Nine ADRs accepted, zero lines of
implementation — which was the point.

## What got decided

| ADR | Decision |
|---|---|
| 0001 | Record architecture decisions |
| 0002 | Separate repository |
| 0003 | Digital-only input, no analog |
| 0004 | Name the project Diatom |
| 0005 | System inclusion criteria |
| 0006 | Keep all cores resident; never unload |
| 0007 | The port interface |
| 0008 | Separate, long-lived process |
| 0009 | The launcher protocol |

Plus, in the register rather than as ADRs: the symbol prefix `diatom_`, the
ten-system curation list, and the consumption model **deferred** with a reason
and a trigger.

## The decision that unlocked everything else

**Excluding Neo Geo.** Not for architectural reasons — Eric simply doesn't want
to play it, and wants everything resident for snappiness.

That single taste call cascaded:

- Peak loaded game dropped to ~10–30 MB against 956 MB, so **residency stopped
  being a design problem** and became obviously correct (ADR-0006).
- Eviction, LRU and tiered policies all became unnecessary.
- `dlclose` never happens, so static TLS, core reload leaks and glibc's refusal
  to return heap became non-problems rather than hazards — and a **phase exit
  criterion was retired**, not skipped.

The lesson worth keeping: a taste decision closed the hardest technical question
in the project. Asking "what do you actually want?" earlier would have saved a
lot of architecture.

## The framing that had to be replaced

"8-bit to 16-bit home console" described the intent but failed as a rule. **GBA
is 6th generation** — later than the PlayStation being excluded — and it's in.
**Neo Geo is 4th generation, 16-bit, 2D and digital** — squarely inside the
framing — and it's out.

Generation predicted two decisions wrongly in opposite directions. ADR-0005
replaced it with four criteria that predict all of them: 2D, digital input,
light enough to stay resident, integer-scales on a 4:3 panel.

## Corrections logged during the phase

Recorded because the count is the evidence that the process worked. Every one was
caught by checking or by pushback, not by being careful.

1. **x86_64 emulation** claimed necessary for the toolchain. `docker manifest
   inspect` showed a multi-arch image that runs arm64 natively. Eric challenged
   it on the grounds that the target device is ARM.
2. **Tailscale SSH** proposed as a fallback for reaching the mini. The server
   side is Linux-only; `tailscale up --ssh` on macOS is a no-op.
3. **Over-tagged `[LB]`** on the host's internal layer model — it is cheap to
   reverse and therefore not load-bearing. Prompted the tag-discipline note in
   register §0 and the three questions for whether a question is ready.
4. **One-core-resident plus process-exit** proposed as the residency design. It
   solved a memory problem that excluding Neo Geo removed. Rejected in ADR-0006
   with the reasoning preserved.
5. **"PCE-CD is much lighter than PS1"** — on the frontend axis, false. The code
   is nearly identical. The real differences are core weight, save-state size,
   multi-disc frequency and CPU load.
6. **"Six-button games map badly onto shoulders"** — wrong. SNES *is* a six-button
   system, four face plus two shoulder, and nobody finds SNES Street Fighter II
   unplayable.
7. **`dt_port_path()`** proposed for save and BIOS directories, violating the
   no-domain-nouns rule stated three paragraphs earlier. Paths come from the
   launcher.
8. **"Host forces one pixel format"** in the §4 table, reversed in ADR-0007 —
   both formats convert free on SDL texture upload, so forcing one would
   sometimes *add* a conversion.

## What Eric caught that changed the outcome

- **No thumbsticks** — became ADR-0003, and made the two devices input-equivalent,
  which resolved the same-curation-both-devices question as a side effect.
- **"Aren't we already at the limit?"** — surfaced that systems ≠ cores.
  `genesis_plus_gx` alone covers three systems.
- **Cairn's burial association** — killed a name I'd recommended.
- **Chert is microcrystalline; diatoms are tiny** — produced the name, and the
  connection is real: chert forms from accumulated diatom silica.
- **`diatom_` over `dia_`** — explicit over terse, and `media_` contains `dia_`.
- **"Does this need testing or do I just say ok?"** — the best question of the
  phase. Produced the three-question framework in register §0 and stopped a
  cheap-to-reverse preference being recorded as a decision.

## What is deliberately unresolved

- **Consumption model** — `[DEFERRED]`. Nothing consumes Diatom yet, so choosing
  an integration model would be choosing without information. Tradeoffs written
  down; subtree currently looks best for co-development.
- **Display handoff between two processes** on fbdev/DRM. Proven on tg5040 by
  PlayOS, will need re-solving per port, likeliest source of platform pain.
- **Core selection** — `gambatte` vs reusing `mgba` for GB/GBC; which `snes9x`
  fork. Both need the Brick, not the Miniloong.
- **Pacing strategy** (§6, §7) — audio-driven with DRC is implied by ADR-0007's
  `audio_queued` and non-blocking writes, but the loop is unspecified.

## Next

Phase 2 — spikes. Three empirical questions, none of which could be answered by
argument:

1. **Environment-call inventory.** Run each core, record every
   `RETRO_ENVIRONMENT_*` it asks for. Converts the long tail from an unknown into
   a checklist, and tells us exactly what the load-time policy struct must hold.
2. **`nm -D`** on `genesis_plus_gx`, `snes9x2010`, and `gambatte` if used.
   Gates ADR-0006's shared-address-space assumption. Workstation, shipping
   tg5040 builds.
3. **Brick measurements** (optional): the 1100 ms breakdown, and RSS with all
   cores mapped and one initialized. ADR-0006's revisit trigger is ~250 MB.
