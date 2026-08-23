# Minimal libretro frontend — scoping register

A living checklist. Nothing here is decided unless it says DECIDED.
Tags: **[LB]** load-bearing (expensive to reverse — decide early) ·
**[OPEN]** needs a decision · **[LATER]** safe to defer.

**On `[LB]` discipline.** Load-bearing means *expensive to reverse* — a
published interface, a data format, something with multiple implementations
depending on it. It does **not** mean "important" or "interesting". Internal
organization you could refactor in an afternoon is not load-bearing, however
much discussion it generates. Several items were over-tagged on 2026-08-23 and
downgraded; a register where everything is load-bearing guides nothing.

Three questions for whether a question is even ready to answer:
1. **What kind of claim is it?** Spec-derivable → read the spec. Runtime
   behaviour → measure. Taste → just decide.
2. **What does being wrong cost?** Cheap to reverse → decide fast and move on.
3. **Is anything blocked on it?** If not, deferring is a legitimate answer, and
   this register exists so deferral doesn't mean forgetting.

---

## 0. Guiding tension

"Abstracted to perfection" = **seams in the right places with nothing leaking
across them**. It does *not* mean maximum layers. On a 1GB device at 60fps,
indirection has a measurable cost. The test for every proposed boundary:

> Does this seam let me swap one real implementation for another real one?

If there will only ever be one implementation behind an interface, it isn't an
abstraction, it's a tax. Two known implementations (desktop + device) is the
bar that justifies a seam.

---

## 0b. Target class, not target devices *(proposed 2026-08-23)*

The frontend should be designed against a **stated device class**, with the
Brick and Miniloong as instances that validate it — not as its definition.
Deriving the envelope from today's two devices bakes them into something meant
to outlive them. Same discipline as §0: one instance is not a class.

Draft class: low-power ARM Linux handheld · ≤1GB RAM · no swap · 4:3-ish panel
that admits integer scaling · SDL2 available · d-pad + face/shoulder buttons,
analog optional.

Chain: **device class → what's possible (ceiling) · taste → what's chosen per
firmware (subset) · chosen set → test matrix + memory envelope.** Devices
constrain; they do not determine. PlayOS runs three systems by curation, not
because the Brick is incapable of more.

- [ ] **[LB]** Ratify or amend the class definition above.
- [x] **[LB]** **Must Diatom survive PS1/Neo-Geo-class workloads? No.**
      → [ADR-0005](decisions/0005-system-inclusion-criteria.md). Peak loaded game
      ~10–30 MB estimated, against 956 MB. This is what makes
      [ADR-0006](decisions/0006-keep-all-cores-resident.md) possible.

Known instances (measured 2026-08-22/23):

| | Brick (tg5040) | Miniloong Pocket 1 |
|---|---|---|
| SoC | Allwinner A133, Cortex-A53 | Rockchip RK3566, Cortex-A55 ×4 |
| RAM | (unmeasured) | 956 MB |
| Panel | 1024×768 | 720×960 portrait → 960×720 landscape |
| OS | NextUI/PlayOS on Linux | Buildroot 2021.11, Linux 5.10 |
| Controls | (unverified — no analog?) | d-pad, A/B/X/Y, L1/R1, L2/R2, Select, Start, Mode, L3, analog |
| Rotation | not needed | **required** |

## 1. Identity and project shape

- [x] **[LB]** Name: **Diatom** →
      **[ADR-0004](decisions/0004-name-the-project-diatom.md)** *(Accepted)*
- [x] **[OPEN]** Collisions checked 2026-08-23 — none in the CFW / libretro
      frontend / handheld firmware space. One accepted out-of-domain hit:
      `diatom-lang/diatom`.
- [x] **[OPEN]** Symbol prefix: **`diatom_`** (2026-08-23). Recorded in
      `CLAUDE.md` as a convention, not an ADR — pervasive but cheap to reverse.
      Explicit over terse, and `dia_` is a substring of `media_`.
- [ ] **[OPEN]** Port naming. `flint` / `slate` / `chalk` for
      tg5040 / miniloong / desktop was proposed when the project was itself a
      stone. Still coherent (diatoms → chert → rock) but no longer automatic.
- [ ] **[LB]** License. 0BSD to match PlayOS is the presumption.
- [ ] **[OPEN]** Confirm the licensing position on `dlopen`-ing GPL cores from a
      permissively licensed frontend. The enabling fact is that `libretro.h` is
      itself permissively licensed, explicitly so any-license frontends can host
      any-license cores. Worth writing down once, properly, rather than
      re-deciding later.
- [x] **[LB]** Standalone repository → **[ADR-0002](decisions/0002-separate-repository.md)** *(Accepted)*
- [x] **[LB]** Decision-recording practice: ADRs → **[ADR-0001](decisions/0001-record-architecture-decisions.md)** *(Accepted)*
- [ ] **[LB]** Consumption model — git submodule, vendored copy, or prebuilt
      artifact? Explicitly left unresolved by ADR-0002; see also §15.
- [x] **[OPEN]** Design work migrated into the `diatom` repository 2026-08-23
      under `docs/`. The scoping folder is gone.

---

## 2. The layer model — "what goes where"

### Vocabulary *(settled 2026-08-23)*

"Frontend" was doing two jobs and caused real confusion. Use:

- **launcher** — the UI the user sees (PlayOS's shelf). Belongs to the firmware.
- **host** — loads and runs a libretro core. **This project.**
- **port** — the per-device platform backend.
- **core** — the emulator plugin. libretro's word; never reused for our code.

### Structure

```
firmware: PlayOS / EROS / future
├── launcher UI ....................... firmware's own, not ours
└── HOST  ← this project
      ├── core loading + env callbacks + run loop   (one module)
      └── port ..... video · audio · input · clock · paths
            ↓
      backend:  desktop │ tg5040 │ miniloong
```

**Only one seam here is load-bearing: host ↔ port.** It has three real
implementations, and getting it wrong means rewriting all of them.

The **host ↔ core** boundary is not ours to design — libretro specifies it. Our
job there is conformance, not architecture.

### Design intent — internal host organization *(not a decision; revisit with code)*

An earlier proposal split "core host" (ABI conformance) from "session" (game
lifecycle, timing, policy) as separate layers. **Current intent: don't.** They
should be one module organized by *file*, not by *layer*.

Reasoning, which is spec-derivable rather than empirical: almost nothing a core
asks via `RETRO_ENVIRONMENT_*` can be answered from ABI knowledge alone.
`GET_SAVE_DIRECTORY`, `SET_PIXEL_FORMAT`, `SET_ROTATION`, `GET_VARIABLE`,
`SET_HW_RENDER`, `GET_INPUT_DEVICE_CAPABILITIES` all need port or session state
— and they arrive from inside `retro_run`, crossing the boundary every frame in
both directions. An interface that must be handed both neighbours' state to
function is a pass-through, not a separation.

**The boundary worth drawing instead is temporal, not architectural:**
*per-game* versus *per-frame*. Resolve all policy — paths, controller type,
pixel format, options, geometry — once at game load into a flat struct the
frame loop reads as plain fields. No allocation, no string lookup, no policy
branching in the hot loop. Cores are permitted to call `GET_VARIABLE` every
frame; a naive implementation will do string comparisons at 60Hz.

*Not an ADR:* it fails the bar in `decisions/README.md` — internal organization,
no external contract, changeable in an afternoon. Verify cheaply by reading
`libretro.h`'s environment enum rather than taking the argument on trust.

*Would be wrong if:* the policy struct needs per-frame mutation. Even then the
answer is a live policy object, not a layer.

- [ ] **[OPEN]** Confirm the above against `libretro.h` when convenient.
- [ ] **[LB]** Which direction do dependencies point? Proposed: strictly
      downward, port never calls up, no callbacks into the host from the port.
- [ ] **[LB]** Does the **host** own the main loop, or the launcher? Resident
      mode + fifo implies the host owns it.
- [ ] **[OPEN]** Is the deliverable a **library**, a **binary**, or both?
- [ ] **[OPEN]** Port selection **compile-time, not runtime** — one binary per
      device, port linked in. Nobody swaps a device backend at runtime; a
      plugin mechanism would be an abstraction with no consumer.

---

## 3. Process boundary

PlayOS today runs `minarch.elf` as a child over a fifo, and the docs cite the
GPL firewall as the reason. If the frontend becomes 0BSD, that reason
evaporates — but two others remain:

- Crash isolation: a bad core takes down the child, not the launcher.
- Memory reclaim: on 1GB, process exit is the only *guaranteed* way to get
  everything back from a leaky core.

- [ ] **[LB]** Keep the separate process, or link the frontend into the
      firmware? Note this interacts with residency: a resident process holding
      cores between games is a very different memory profile from a process per
      game.
- [ ] **[OPEN]** If separate: what is the IPC protocol? (fifo today — keep,
      formalize, or replace?)
- [ ] **[OPEN]** PlayOS constraint to respect: the boot hook powers the device
      **off** if the launch loop exits. Any process model must not trip that.

---

## 4. Port interface (the most important seam)

Candidate surface — deliberately small:

- **Video**: init(geometry), present(frame, w, h, pitch, format), geometry
  change, teardown
- **Audio**: open(sample_rate), queue(samples, count), queued_frames(), close
- **Input**: poll(), state(port, device, id)
- **System**: monotonic clock, sleep, paths, log
- **Optional/device**: brightness, LED, CPU governor

- [ ] **[LB]** Is that the complete surface? What's missing that will otherwise
      leak device knowledge upward?
### Proposed division of labour *(2026-08-23, not ratified)*

| Concern | Host (shared) | Port (device) |
|---|---|---|
| Scaling | compute factor + offsets from core geometry vs surface | perform the blit / present |
| Rotation | **nothing — never knows** | everything |
| Pixel format | force one format on cores | convert only if the panel demands otherwise |
| Input | map canonical buttons → retropad | produce canonical buttons |
| Audio | resample to the rate the port reports | report native rate, accept PCM |
| Timing | pace against core fps | monotonic clock, present |

The scaling split is the point: **deciding the factor is arithmetic** (shared,
so every port behaves identically), **performing the blit is hardware**. Put
both in the port and the math gets duplicated per device and drifts.

Rotation is the test of whether the seam holds. The Miniloong's framebuffer is
720×960 portrait; if anything above the port learns that, the abstraction has
leaked.

**Anti-pattern:** the port interface will want to grow. If a port function's
name contains a domain noun — game, save, core, menu — it is in the wrong
layer. The port deals in pixels, samples, buttons, time, paths. Nothing else.
Every concept admitted to the port interface must be reimplemented per backend,
including desktop — which is how the desktop backend rots and stops being
usable for iteration.

- [x] **[LB]** Ratified and superseded in detail by
      **[ADR-0007](decisions/0007-port-interface.md)** *(Accepted)* — ten
      functions; the port must never include `libretro.h`.

Two amendments ADR-0007 makes to the table above:

- **Pixel format passes through** (accept RGB565 + XRGB8888, refuse 0RGB1555)
  rather than the host forcing one on cores. Both convert free on SDL texture
  upload, so forcing would sometimes *add* a conversion.
- **Paths leave the port entirely.** `save` and `system` are domain nouns. The
  launcher supplies paths to the host at startup.

- [x] **[OPEN]** Does the port ever get to refuse a geometry? No — the host
      computes `dst` and the port blits. Scale policy never reaches the port.
- [ ] **[OPEN]** Does the port ever get to *refuse* — e.g. "this geometry can't
      be integer-scaled on this panel"? Who handles that?
- [ ] **[OPEN]** Backends to build: `desktop` (SDL2, first), `tg5040` (Brick),
      Miniloong later.
- [ ] **[LB]** SDL2 as the baseline for both desktop and device, or SDL2 on
      desktop and something lower on device?

---

## 5. Video

- [ ] **[OPEN]** Force a single pixel format on cores, or support all three?
- [ ] **[LB]** Integer-scale-only as a hard rule? Brick's 1024×768 makes 3×/4×/5×
      work for essentially every 2D system. Other panels (640×480, 720×720,
      1280×720) may break it.
- [ ] **[OPEN]** What happens when integer scaling doesn't fit — refuse, letterbox
      at a lower factor, or relax the rule per device?
- [ ] **[OPEN]** Aspect-ratio and overscan policy. Crop, or show everything?
- [ ] **[OPEN]** Rotation support (some panels are physically rotated).
- [ ] **[LATER]** Zero-copy: `GET_CURRENT_SOFTWARE_FRAMEBUFFER` lets some cores
      render straight into your texture. Real RAM/bandwidth win, not day one.
- [ ] **[OPEN]** GL/GLES or software blit on device?
- [ ] **[LATER]** Shaders/overlays — probably "no" forever. Decide and write it down.

---

## 6. Audio — expect this to be the hard part

- [ ] **[LB]** Sync strategy: audio-driven, video-driven, or **dynamic rate
      control** (nudge the resample ratio to keep the buffer centered). DRC is
      what the mature frontends do.
- [ ] **[LB]** Resampler: libsamplerate (what minarch uses) or hand-rolled
      linear/cubic? Check libsamplerate's current license before depending on it.
- [ ] **[OPEN]** Target output rate. Fixed 48k, or follow the device?
- [ ] **[OPEN]** Buffer size — latency vs underrun tolerance.
- [ ] **[OPEN]** Behaviour when a frame overruns budget: drop audio, stretch,
      or let it underrun?
- [ ] **[OPEN]** GBA is the best test case (59.7275Hz, awkward rate). Make it
      the audio conformance target.

---

## 7. Timing and pacing

- [ ] **[LB]** Pace to the **core's** rate (from `retro_get_system_av_info`,
      e.g. 59.7275) or the **panel's**? They disagree on every system.
- [ ] **[OPEN]** Frame drop/duplicate policy when they disagree.
- [ ] **[OPEN]** Miniloong's 120Hz panel is a clean 2× — does that change the
      answer per device, and does the port get a say?
- [ ] **[OPEN]** vsync: available on these panels? Tearing acceptable?

---

## 8. Input

- [ ] **[LB]** Where does physical→retropad mapping live — port, config, or
      session?
- [ ] **[OPEN]** Per-core remapping, or one map?
- [ ] **[OPEN]** Who owns hotkeys (menu, brightness, volume) — frontend or
      firmware? PlayOS currently keeps L3/R3 away from cores because the Brick's
      F1/F2 report as those. That's device knowledge that must not leak upward.
- [x] **[LB]** Analog sticks: **no analog support at all** →
      **[ADR-0003](decisions/0003-digital-only-input.md)** *(Accepted)*
- [x] **[OPEN]** Button count is **not** a constraint. Both devices give 4 face
      + 4 shoulder; six is the maximum any in-scope system asks (SNES, 6-button
      Genesis, CPS2), covered by the standard 4+L1/R1 mapping — the same layout
      SNES uses. An earlier claim that six-button games map "badly" was wrong.
- [x] **[OPEN]** Same curation on both devices? **Probably yes.** With no analog,
      the two devices are input-equivalent, so one test matrix rather than two.
      (Follows from ADR-0003; revisit if the class widens.)
- [ ] **[LB]** Controller **device-type** selection is still required —
      `retro_set_controller_port_device`. Genesis 3- vs 6-button is a
      correctness issue, not a preference; PC Engine has the same 2- vs 6-button
      split. Digital-only removes axes, **not** device types. Where does the
      type live — firmware config, per-ROM, or core default?
- [ ] **[OPEN]** Verify the Brick's physical controls (unmeasured). No longer
      blocking after ADR-0003, but §0b's device table is incomplete without it.

### Curation notes (not ADR material — curation lives in firmware config)

- **Genesis is in** (2026-08-23). Passes the no-sticks filter, ≤6 buttons, ROMs
  typically 1–4 MB so it stays in the cheap tier. Launched 3-button in 1988; the
  6-button pad arrived 1993 and most of the library predates it.

---

## 9. State and storage

- [ ] **[OPEN]** SRAM write policy: on exit, periodic, on menu open?
- [ ] **[OPEN]** Save state format and slot convention (PlayOS autosaves slot 9).
- [ ] **[OPEN]** Preview screenshots for the launcher — frontend's job or host's?
- [ ] **[LB]** Rewind: support or drop? Real RAM cost on a 1GB device.
- [ ] **[OPEN]** Who owns paths — host passes them in, presumably.
- [ ] **[LATER]** CHD/CD support. Only matters if disc systems are in scope.

---

## 10. Residency and lifecycle — the genuinely novel part

- [ ] **[LB]** Policy: keep-one, keep-N with LRU, or tiered by core weight?
- [ ] **[LB]** Eviction trigger: core count, or measured RSS against a budget?
- [ ] **[OPEN]** **Does `retro_deinit` + `dlclose` actually return the memory?**
      Some cores hold static state or leak on repeated load. This needs
      *measuring*, not assuming — and the answer may differ per core. Could
      invalidate the whole residency design, so test early.
- [ ] **[OPEN]** Is repeated load/unload of the same core even safe for all
      target cores?
- [ ] **[OPEN]** What's the observable win? PlayOS got 1100ms → 200ms with three
      cores resident. Measure before rebuilding it.

### RESOLVED 2026-08-23 → [ADR-0006](decisions/0006-keep-all-cores-resident.md)

**`dlopen` every core and never `dlclose`. `retro_init` only the one in use.**

A proposal of 2026-08-22 — *one core resident, exit the process on system
switch* — was **rejected**. It bounded memory when Neo Geo was still a
candidate; [ADR-0005](decisions/0005-system-inclusion-criteria.md) excluded Neo
Geo, removing the problem it solved. It also paid ~170 ms plus a process restart
on every system switch, against the snappiness goal.

Eviction, LRU and tiering are all unnecessary. The three `dlclose` hazards —
static TLS silently preventing unload, cores leaking across reloads, glibc not
returning heap to the OS — become non-problems rather than risks, because the
operation never occurs.

- [x] **[LB]** Residency policy → ADR-0006 *(Accepted)*
- [x] **[OPEN]** `dlclose` measurement — **no longer required.** Retired from the
      phase exit criteria; Diatom never calls it.
- [ ] **[OPEN]** Still worth measuring eventually, but nothing is blocked on it:
      (a) breakdown of PlayOS's 1100 ms — process+SDL+GL vs `dlopen` vs game load;
      (b) RSS with all cores mapped and one initialized (ADR-0006 revisit
      trigger is ~250 MB); (c) `dlopen` cost per core.
      **Must be measured on the Brick** — the Miniloong is Cortex-A55 against the
      Brick's A53 and would flatter any timing result.
- [ ] **[LB]** `nm -D` check on `snes9x2010`, `genesis_plus_gx`, `gambatte`
      before implementation. Runs on a workstation against the **shipping
      tg5040 builds** — `nm` inspects a binary and needs no device, and another
      device's builds prove nothing since exports depend on build flags.

---

## 11. Memory discipline (the project's thesis)

- [ ] **[LB]** Set an explicit **RSS budget** and assert it in tests.
- [ ] **[OPEN]** Arena/static allocation; no malloc in the frame loop.
- [ ] **[OPEN]** How do you account for core-side allocations you don't control?
- [ ] **[OPEN]** Measurement harness — what tool, what granularity?

---

## 12. Core options and config

- [ ] **[OPEN]** Expose core options at all, or hardcode a curated set per core?
- [ ] **[OPEN]** If exposed, who renders the UI — frontend or firmware?
- [ ] **[OPEN]** Config format and ownership (host-owned is the presumption).

---

## 13. Testing and dev loop

- [ ] **[LB]** Desktop backend **first**, before any device work.
- [ ] **[OPEN]** Environment-call logging shim — run each target core, record
      every `RETRO_ENVIRONMENT_*` it asks for. Turns the unknown long tail into
      a finite checklist. Cheap, high value, useful even if the rewrite is
      abandoned.
- [ ] **[OPEN]** Headless conformance test: run N frames, checksum framebuffer,
      assert RSS ceiling.
- [ ] **[LATER]** CI.

---

## 14. Failure handling

- [ ] **[OPEN]** Core crash behaviour (interacts with §3).
- [ ] **[OPEN]** Missing BIOS, bad ROM, unsupported geometry — fail how?
- [ ] **[OPEN]** Never trip PlayOS's power-off failsafe.

---

## 15. Build and consumption

- [ ] **[OPEN]** C standard (PlayOS uses gnu11) and toolchain container reuse.
- [ ] **[OPEN]** How firmwares consume it (see §1).
- [ ] **[OPEN]** Cross-device build matrix.

---

## Open questions needing Eric, not analysis

1. What systems are actually on your card — Brick and Miniloong?
2. What do the controls physically allow on each device?
3. Same curation on both devices, or different per device?
4. Is integer-scale-only a principle, or a Brick luxury?
