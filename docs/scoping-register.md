# Minimal libretro frontend - scoping register

A living checklist. Nothing here is decided unless it says DECIDED.
Tags: **[LB]** load-bearing (expensive to reverse - decide early) ·
**[OPEN]** needs a decision · **[LATER]** safe to defer ·
**[DEFERRED]** was load-bearing, deliberately postponed with a written reason
and a trigger for revisiting - distinct from `[LATER]`, which was never urgent.

**On `[LB]` discipline.** Load-bearing means *expensive to reverse* - a
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
Brick and Miniloong as instances that validate it - not as its definition.
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
      ~10-30 MB estimated, against 956 MB. This is what makes
      [ADR-0006](decisions/0006-keep-all-cores-resident.md) possible.

Known instances (measured 2026-08-22/23):

| | Brick (TG3040) | Miniloong Pocket 1 |
|---|---|---|
| SoC | Cortex-A53 (`CPU part 0xd03`, measured) | Rockchip RK3566, Cortex-A55 ×4 |
| RAM | **975 MB** (998,332 kB, measured) | 956 MB (978,824 kB, measured) |
| Swap | **0** (measured) | 0 |
| Kernel | 4.9.191 | 5.10 |
| Panel | 1024×768 | 720×960 portrait → 960×720 landscape |
| Controls | (unverified - no analog?) | d-pad, A/B/X/Y, L1/R1, L2/R2, Select, Start, Mode, L3, analog |
| Rotation | not needed | **required** |

Both devices measured 2026-08-22/23. **Zero swap on both** - the fact underneath
the file-backed-versus-anonymous reasoning in ADR-0006 and ADR-0010, now
verified rather than assumed.

## 1. Identity and project shape

- [x] **[LB]** Name: **Diatom** →
      **[ADR-0004](decisions/0004-name-the-project-diatom.md)** *(Accepted)*
- [x] **[OPEN]** Collisions checked 2026-08-23 - none in the CFW / libretro
      frontend / handheld firmware space. One accepted out-of-domain hit:
      `diatom-lang/diatom`.
- [x] **[OPEN]** Symbol prefix: **`diatom_`** (2026-08-23). Recorded in
      `working-agreement.md` as a convention, not an ADR - pervasive but cheap to reverse.
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
- [ ] **[DEFERRED]** Consumption model - submodule, subtree, vendored copy, or
      prebuilt artifact. **Deliberately deferred 2026-08-23: nothing consumes
      Diatom yet.** Choosing an integration model before either side exists means
      choosing without the information that makes it obvious. Decide when PlayOS
      actually needs to embed it, by which point how often the two change
      together will be known.

      Notes for when it is revisited:
      - **Submodule** - pin is structural, but detached HEAD, `--recursive` on
        clone, and a two-step pointer bump that is silently easy to forget.
        Worst precisely during co-development, which is the phase this will be
        in for months.
      - **Subtree** - same files-in-tree result as vendoring, but git records the
        provenance instead of a README claiming it. `--squash` keeps the log
        clean. Better for co-development: edit in place, `subtree push` upstream.
      - **Manual vendoring** - what `minarch/overrides/` does today, and the
        reason PlayOS's docs claim a `v6.11.2` baseline while the build actually
        reads `v6.11.2-10-g96eeacd9`. The provenance lives in prose and drifts.
      - Weaker argument than it first appeared: the NextUI drift was a *third
        party* moving underneath. Diatom is Eric's - it only changes when he
        changes it.
- [x] **[OPEN]** Design work migrated into the `diatom` repository 2026-08-23
      under `docs/`. The scoping folder is gone.

---

## 2. The layer model - "what goes where"

### Vocabulary *(settled 2026-08-23)*

"Frontend" was doing two jobs and caused real confusion. Use:

- **launcher** - the UI the user sees (PlayOS's shelf). Belongs to the firmware.
- **host** - loads and runs a libretro core. **This project.**
- **port** - the per-device platform backend.
- **core** - the emulator plugin. libretro's word; never reused for our code.

### Structure

```
firmware: PlayOS / EROS / future
├── launcher UI ....................... firmware's own, not ours
└── HOST  ← this project
      ├── core loading + env callbacks + run loop   (one module)
      └── port ..... video · audio · input · clock · paths
            ↓
      backend:  desktop │ brick │ miniloong
```

**Only one seam here is load-bearing: host ↔ port.** It has three real
implementations, and getting it wrong means rewriting all of them.

The **host ↔ core** boundary is not ours to design - libretro specifies it. Our
job there is conformance, not architecture.

### Design intent - internal host organization *(not a decision; revisit with code)*

An earlier proposal split "core host" (ABI conformance) from "session" (game
lifecycle, timing, policy) as separate layers. **Current intent: don't.** They
should be one module organized by *file*, not by *layer*.

Reasoning, which is spec-derivable rather than empirical: almost nothing a core
asks via `RETRO_ENVIRONMENT_*` can be answered from ABI knowledge alone.
`GET_SAVE_DIRECTORY`, `SET_PIXEL_FORMAT`, `SET_ROTATION`, `GET_VARIABLE`,
`SET_HW_RENDER`, `GET_INPUT_DEVICE_CAPABILITIES` all need port or session state
- and they arrive from inside `retro_run`, crossing the boundary every frame in
both directions. An interface that must be handed both neighbours' state to
function is a pass-through, not a separation.

**The boundary worth drawing instead is temporal, not architectural:**
*per-game* versus *per-frame*. Resolve all policy - paths, controller type,
pixel format, options, geometry - once at game load into a flat struct the
frame loop reads as plain fields. No allocation, no string lookup, no policy
branching in the hot loop. Cores are permitted to call `GET_VARIABLE` every
frame; a naive implementation will do string comparisons at 60Hz.

*Not an ADR:* it fails the bar in `decisions/README.md` - internal organization,
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
- [ ] **[LB]** **Standalone operation as the primary mode.** Diatom should run
      with no host at all:

      ```
      diatom --core snes9x_libretro.so --rom parodius.sfc
      ```

      That is how the desktop backend runs, how it is tested, and how a
      prospective consumer evaluates it without adopting a firmware. The
      launcher protocol ([ADR-0009](decisions/0009-launcher-protocol.md)) then
      becomes an **embedding mode** for hosts wanting a resident process -
      an additional interface, not the fundamental one.

      Does not supersede ADR-0008 or ADR-0009; both stand. Designing for a
      program that stands alone is a stricter test than designing for one
      embedder, and it is what keeps Diatom from being a component of somebody
      else's launcher.
- [ ] **[OPEN]** Port selection **compile-time, not runtime** - one binary per
      device, port linked in. Nobody swaps a device backend at runtime; a
      plugin mechanism would be an abstraction with no consumer.

---

## 3. Process boundary

PlayOS today runs `minarch.elf` as a child over a fifo, and the docs cite the
GPL firewall as the reason. If the frontend becomes 0BSD, that reason
evaporates - but two others remain:

- Crash isolation: a bad core takes down the child, not the launcher.
- Memory reclaim: on 1GB, process exit is the only *guaranteed* way to get
  everything back from a leaky core.

- [x] **[LB]** Separate, **long-lived** process →
      **[ADR-0008](decisions/0008-separate-long-lived-process.md)** *(Accepted)*.
      The GPL reason is gone; what carries it now is that a core segfault must
      not take the launcher down, because the boot hook powers the device off
      when the launch loop exits.
- [x] **[LB]** Protocol → **[ADR-0009](decisions/0009-launcher-protocol.md)**
      *(Accepted)*. One Unix domain socket, line-based tab-separated
      `key=value`. Collapses five mechanisms (two fifos, pid file, temp file,
      signal) into one. `ERROR` = never started; `EXIT` = ran and stopped -
      a rule about **display ownership**, not error reporting.
- [x] **[OPEN]** The power-off failsafe is respected: the launcher stays alive
      as supervisor, and `SIGUSR1` is retained as the escape hatch for a core
      wedged inside `retro_run` that cannot read the socket.
- [ ] **[OPEN]** Remaining risk carried by ADR-0008: **display handoff between
      two processes** on fbdev/DRM. Proven on tg5040 by PlayOS today; will need
      re-solving per port and is the likeliest source of platform-specific pain.

---

## 4. Port interface (the most important seam)

Candidate surface - deliberately small:

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
| Rotation | **nothing - never knows** | everything |
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
name contains a domain noun - game, save, core, menu - it is in the wrong
layer. The port deals in pixels, samples, buttons, time, paths. Nothing else.
Every concept admitted to the port interface must be reimplemented per backend,
including desktop - which is how the desktop backend rots and stops being
usable for iteration.

- [x] **[LB]** Ratified and superseded in detail by
      **[ADR-0007](decisions/0007-port-interface.md)** *(Accepted)* - ten
      functions; the port must never include `libretro.h`.

Two amendments ADR-0007 makes to the table above:

- **Pixel format passes through** (accept RGB565 + XRGB8888, refuse 0RGB1555)
  rather than the host forcing one on cores. Both convert free on SDL texture
  upload, so forcing would sometimes *add* a conversion.
- **Paths leave the port entirely.** `save` and `system` are domain nouns. The
  launcher supplies paths to the host at startup.

- [x] **[OPEN]** Does the port ever get to refuse a geometry? No - the host
      computes `dst` and the port blits. Scale policy never reaches the port.
- [ ] **[OPEN]** Does the port ever get to *refuse* - e.g. "this geometry can't
      be integer-scaled on this panel"? Who handles that?
- [ ] **[OPEN]** Backends to build: `desktop` (SDL2, first), `brick` (TG3040),
      Miniloong later.
- [ ] **[LB]** SDL2 as the baseline for both desktop and device, or SDL2 on
      desktop and something lower on device?

---

## 5. Video

- [ ] **[OPEN]** Force a single pixel format on cores, or support all three?
- [ ] **[LB]** Integer-scale-only as a hard rule? Brick's 1024×768 makes 3×/4×/5×
      work for essentially every 2D system. Other panels (640×480, 720×720,
      1280×720) may break it.
- [ ] **[OPEN]** What happens when integer scaling doesn't fit - refuse, letterbox
      at a lower factor, or relax the rule per device?
- [ ] **[OPEN]** Aspect-ratio and overscan policy. Crop, or show everything?
- [ ] **[OPEN]** Rotation support (some panels are physically rotated).
- [ ] **[LATER]** Zero-copy: `GET_CURRENT_SOFTWARE_FRAMEBUFFER` - **confirmed
      2026-08-23: FCEUmm requests it every frame.** The path ADR-0007 deferred
      is actively offered. Still deferred; good to know it is real.
- [x] **[OPEN]** Pixel format - **all six cores chose RGB565** (measured). The
      XRGB8888 path ADR-0007 accepts is currently dead code; do not build or
      test it until something needs it.
- [x] **[OPEN]** Geometry changes mid-run are **common, not exotic** - 3/6 cores
      call `SET_GEOMETRY` during `run`. Snes9x `max 604×478` vs `base 256×224`;
      Beetle PCE `max 512×243`. Confirms recompute-on-change.
- [ ] **[OPEN]** **PC Engine breaks integer scale on one device but not the
      other.** 256×**243** at 3× is 768×**729** - exceeds the Miniloong's 720
      lines, fits the Brick's 768. So PCE gets 2× on one and 3× on the other.
      First real instance of ADR-0007's "doesn't fit" default.
- [ ] **[OPEN]** GL/GLES or software blit on device?
- [ ] **[LATER]** Shaders/overlays - probably "no" forever. Decide and write it down.

---

## 6. Audio - expect this to be the hard part

### Measured 2026-08-23 ([spike](spikes/2026-08-23-env-inventory.md))

| Core | fps | Sample rate |
|---|---|---|
| FCEUmm (PAL) | **50.0070** | 48000 |
| Snes9x (PAL) | **50.0070** | **32040** |
| Gambatte | 59.7275 | 32768 |
| PicoDrive | 60.0000 | 44100 |
| Beetle PCE | 59.8200 | 44100 |
| mGBA | 59.7275 | **65536** |

**Five distinct frame rates, five distinct sample rates.** Only PicoDrive hits
exactly 60. mGBA emits 65536 Hz, above any device rate. **DRC is mandatory, not
a refinement** - there is no configuration in which rates line up.

Two levers the spike discovered:

- **`GET_TARGET_SAMPLE_RATE`** (cmd 81) - a core asks what rate we want.
  Answering lets it generate at the device rate natively and skip resampling
  for that core entirely.
- **`SET_AUDIO_BUFFER_STATUS_CALLBACK`** (cmd 62, 3/6 cores) - cores offer to
  *receive* buffer occupancy and throttle themselves. The core-side half of DRC.

- [ ] **[LB]** Sync strategy: audio-driven, video-driven, or **dynamic rate
      control** (nudge the resample ratio to keep the buffer centered). DRC is
      what the mature frontends do - and per the table above, unavoidable.
- [ ] **[OPEN]** Implement `GET_TARGET_SAMPLE_RATE` and/or
      `SET_AUDIO_BUFFER_STATUS_CALLBACK`, or resample everything centrally?
- [ ] **[LB]** Resampler: libsamplerate (what minarch uses) or hand-rolled
      linear/cubic? Check libsamplerate's current license before depending on it.
- [ ] **[OPEN]** Target output rate. Fixed 48k, or follow the device?
- [ ] **[OPEN]** Buffer size - latency vs underrun tolerance.
- [ ] **[OPEN]** Behaviour when a frame overruns budget: drop audio, stretch,
      or let it underrun?
- [x] **[OPEN]** ~~GBA is the best test case.~~ Superseded by measurement: the
      hardest case is **PAL at 50.0070 Hz on a 60 Hz panel** (FCEUmm, Snes9x),
      and the most awkward *rate* is mGBA's **65536 Hz**. Use both as
      conformance targets, not GBA alone.

---

## 7. Timing and pacing

**This is now the most interesting open problem in the project.** Measured
2026-08-23: five distinct core frame rates, and **PAL at 50.0070 Hz is a
deliberate target**, not an edge case - Probotector is PAL-only Contra. Pacing
50 Hz content on a 60 Hz panel is the *normal* case for part of the library.

- [ ] **[LB]** Pace to the **core's** rate (from `retro_get_system_av_info`) or
      the **panel's**? Measured disagreement: 50.0070 · 59.7275 · 59.8200 ·
      60.0000. Only PicoDrive matches a 60 Hz panel.
- [ ] **[OPEN]** Frame drop/duplicate policy when they disagree.
- [ ] **[OPEN]** Miniloong's 120Hz panel is a clean 2× - does that change the
      answer per device, and does the port get a say?
- [ ] **[OPEN]** vsync: available on these panels? Tearing acceptable?

---

## 8. Input

- [ ] **[LB]** Where does physical→retropad mapping live - port, config, or
      session?
- [ ] **[OPEN]** Per-core remapping, or one map?
- [ ] **[OPEN]** Who owns hotkeys (menu, brightness, volume) - frontend or
      firmware? PlayOS currently keeps L3/R3 away from cores because the Brick's
      F1/F2 report as those. That's device knowledge that must not leak upward.
- [x] **[LB]** Analog sticks: **no analog support at all** →
      **[ADR-0003](decisions/0003-digital-only-input.md)** *(Accepted)*
- [x] **[OPEN]** Button count is **not** a constraint. Both devices give 4 face
      + 4 shoulder; six is the maximum any in-scope system asks (SNES, 6-button
      Genesis, CPS2), covered by the standard 4+L1/R1 mapping - the same layout
      SNES uses. An earlier claim that six-button games map "badly" was wrong.
- [x] **[OPEN]** Same curation on both devices? **Probably yes.** With no analog,
      the two devices are input-equivalent, so one test matrix rather than two.
      (Follows from ADR-0003; revisit if the class widens.)
- [ ] **[LB]** Controller **device-type** selection is still required -
      `retro_set_controller_port_device`. Genesis 3- vs 6-button is a
      correctness issue, not a preference; PC Engine has the same 2- vs 6-button
      split. Digital-only removes axes, **not** device types. Where does the
      type live - firmware config, per-ROM, or core default?
- [ ] **[OPEN]** Verify the Brick's physical controls (unmeasured). No longer
      blocking after ADR-0003, but §0b's device table is incomplete without it.

### Test matrix - *not* a core list

**Diatom has no core list and no system list.** It loads whatever core it is
handed. The table below is the set Diatom is **verified against**, so its
envelope and geometry assumptions have evidence behind them. Which core actually
covers which system is the host application's config decision - for PlayOS,
`systems.cfg`. See `working-agreement.md`.

Systems in scope by the criteria in
[ADR-0005](decisions/0005-system-inclusion-criteria.md); the list moves without
superseding it.

| System | Max ROM | Integer scale |
|---|---|---|
| NES | ~1 MB | 3× → 768×720 |
| Master System | ~1 MB | 4× on Brick (1024×768 exact), 3× on Miniloong |
| Game Gear | ~1 MB | 5× → 800×720 |
| PC Engine (HuCard) | ~1 MB | 3× → 768×717 |
| PC Engine CD | - | 3× |
| Genesis | 4 MB (8 max) | 3× → 960×672 |
| SNES | 6 MB | 3× → 768×672 |
| Game Boy / GBC | ~8 MB | 5× → 800×720 |
| GBA | 32 MB | 4× → 960×640 |

Largest ROM is GBA's 32 MB, so the
[ADR-0006](decisions/0006-keep-all-cores-resident.md) envelope holds regardless
of which cores a host chooses. **Cores, not systems, are the cost unit** - one
core routinely covers several systems, so adding a system is often free.

**Cores verified against so far** (2026-08-23, see the
[env-inventory spike](spikes/2026-08-23-env-inventory.md)): `fceumm`,
`gambatte`, `snes9x` 1.63, `picodrive`, `mednafen_pce_fast`, `mgba`.

These six were a **convenience sample** - binaries already on disk from a NextUI
release - not a recommendation and not the available set. libretro cores are
independent upstream binaries; Eric already builds his own via `libretro-super`.
Substituting `genesis_plus_gx` for `picodrive`, or a lighter `snes9x` fork, is a
host decision and only means re-running the spike, which is cheap.

- [ ] **[OPEN]** Verify against a second core for at least one system, to prove
      nothing in Diatom is tuned to a particular core's behaviour.
- [ ] **[OPEN]** SNES coprocessors - SuperFX (Star Fox, Yoshi's Island), SA-1,
      DSP-1, CX4 cost CPU, not RAM, and are the known pinch point on A53-class
      hardware. Whether a given core keeps up **must be judged on the Brick**,
      not the Miniloong. A host-side core-choice question, but Diatom's test
      matrix should include whichever gets chosen.

**Genesis note:** launched 3-button in 1988; the 6-button pad arrived 1993 and
most of the library predates it. Both fit 4 face + L1/R1. Requires
`retro_set_controller_port_device` - some early games misbehave with a 6-button
pad attached, which is why the real pad has a Mode switch.

---

## 9. State and storage

- [ ] **[OPEN]** SRAM write policy: on exit, periodic, on menu open?
- [ ] **[OPEN]** Save state format and slot convention (PlayOS autosaves slot 9).
- [ ] **[OPEN]** Preview screenshots for the launcher - frontend's job or host's?
- [ ] **[LB]** Rewind: support or drop? Real RAM cost on a 1GB device.
- [ ] **[OPEN]** Who owns paths - host passes them in, presumably.
- [ ] **[LATER]** CHD/CD support. Only matters if disc systems are in scope.

---

## 10. Residency and lifecycle - the genuinely novel part

- [ ] **[LB]** Policy: keep-one, keep-N with LRU, or tiered by core weight?
- [ ] **[LB]** Eviction trigger: core count, or measured RSS against a budget?
- [ ] **[OPEN]** **Does `retro_deinit` + `dlclose` actually return the memory?**
      Some cores hold static state or leak on repeated load. This needs
      *measuring*, not assuming - and the answer may differ per core. Could
      invalidate the whole residency design, so test early.
- [ ] **[OPEN]** Is repeated load/unload of the same core even safe for all
      target cores?
- [ ] **[OPEN]** What's the observable win? PlayOS got 1100ms → 200ms with three
      cores resident. Measure before rebuilding it.

### RESOLVED 2026-08-23 → [ADR-0006](decisions/0006-keep-all-cores-resident.md)

**`dlopen` every core and never `dlclose`. `retro_init` only the one in use.**

A proposal of 2026-08-22 - *one core resident, exit the process on system
switch* - was **rejected**. It bounded memory when Neo Geo was still a
candidate; [ADR-0005](decisions/0005-system-inclusion-criteria.md) excluded Neo
Geo, removing the problem it solved. It also paid ~170 ms plus a process restart
on every system switch, against the snappiness goal.

Eviction, LRU and tiering are all unnecessary. The three `dlclose` hazards -
static TLS silently preventing unload, cores leaking across reloads, glibc not
returning heap to the OS - become non-problems rather than risks, because the
operation never occurs.

- [x] **[LB]** Residency policy → ADR-0006 *(Accepted)*
- [x] **[OPEN]** `dlclose` measurement - **no longer required.** Retired from the
      phase exit criteria; Diatom never calls it.
- [x] **[OPEN]** Measured on the Brick 2026-08-23 →
      [spike result](spikes/2026-08-23-rss-and-dlopen.md).

      **6 cores mapped + 1 running = 15.0 MB.** Trigger is 250 MB; RAM is 975 MB.
      Residency was never close to a memory problem - ~2 MB resident per mapped
      core, ~2 MB more for the loaded game.

      **The ADR-0006 estimate was wrong by 10×** (~150 MB predicted). A mapped
      `.so` costs far less RSS than its file size, because only touched pages
      become resident. Wrong in the safe direction, but it was a guess with a
      number attached.

      **The trigger is badly calibrated as a result** - at 250 MB against a
      150 MB estimate, it needs a 16× regression to fire. ~50 MB would mean
      something. Noted rather than superseding ADR-0006, whose decision is
      confirmed.

      `dlopen`: **232 ms cold for all six, 35 ms warm.** Page-cache state changes
      time, not RSS. Does **not** reproduce PlayOS's `~170 ms` per-core figure
      (worst cold case here is 70 ms) - recorded as a discrepancy, since the two
      measurements differ in context and I cannot say why from here.

      FCEUmm executes at **1.81 ms/frame** against a 20.0 ms PAL budget - ~9%,
      core execution only, no scaling/blit/audio. RSS identical after 120 and
      720 frames: no leak observed in that span.
- [x] **[LB]** `nm -D` check - **DONE 2026-08-23. ADR-0006's revisit trigger
      fired.** → **[ADR-0010](decisions/0010-rtld-local-is-mandatory.md)**
      *(Accepted)*

      | Core | Exports | `retro_*` | Other |
      |---|---|---|---|
      | fceumm | 45 | 45 | 0 |
      | gambatte | 46 | 46 | 0 |
      | mednafen_pce_fast | 53 | 53 | 0 |
      | mgba | 25 | 25 | 0 |
      | snes9x | 27 | 27 | 0 |
      | **picodrive** | **1115** | 46 | **1069** |

      PicoDrive exports a complete statically-linked zlib (`crc32`, `inflate`,
      `deflate`, `gzopen`, …) plus ~1000 internal names generic enough to
      collide - `Pico`, `cdd`, `ssp`, `decode`, `tcache`, `MyFree`, `g_argv`.

      **ADR-0006's decision stands; one supporting claim does not.** The `nm`
      check was described there as proving an empty collision surface, "a
      stronger guarantee" than `RTLD_LOCAL`. False in general. `RTLD_LOCAL` is
      the *only* mechanism - hence ADR-0010.

      A property of the **build**, not the emulator: the same source with
      `-fvisibility=hidden` would be clean. No core's exports can be assumed.
      The check is retained as advisory.

      fceumm 45, mednafen_pce_fast 53 and mgba 25 match PlayOS's earlier counts
      exactly - independent corroboration.

---

## 11. Memory discipline (the project's thesis)

- [ ] **[LB]** Set an explicit **RSS budget** and assert it in tests.
- [ ] **[OPEN]** Arena/static allocation; no malloc in the frame loop.
- [ ] **[OPEN]** How do you account for core-side allocations you don't control?
- [ ] **[OPEN]** Measurement harness - what tool, what granularity?

---

## 12. Core options and config

- [ ] **[OPEN]** Expose core options at all, or hardcode a curated set per core?
- [ ] **[OPEN]** If exposed, who renders the UI - frontend or firmware?
- [ ] **[OPEN]** Config format and ownership (host-owned is the presumption).

---

## 13. Testing and dev loop

- [ ] **[LB]** Desktop backend **first**, before any device work.
- [x] **[OPEN]** Environment-call logging shim - **DONE 2026-08-23** →
      [spike result](spikes/2026-08-23-env-inventory.md). All six cores, real
      ROMs, full lifecycle, run in an aarch64 container with no device involved.

      **34 of 77 commands appear · ~17 must be implemented · 17 declined by every
      core with nothing breaking · 43 never appear.** The long tail is a
      checklist.

      Consequences recorded below in §5, §6, §7 and §12.
- [ ] **[OPEN]** Headless conformance test: run N frames, checksum framebuffer,
      assert RSS ceiling.
- [ ] **[LATER]** CI.

---

## 14. Failure handling

- [ ] **[OPEN]** Core crash behaviour (interacts with §3).
- [ ] **[OPEN]** Missing BIOS, bad ROM, unsupported geometry - fail how?
- [ ] **[OPEN]** Never trip PlayOS's power-off failsafe.

---

## 15. Build and consumption

- [ ] **[OPEN]** C standard (PlayOS uses gnu11) and toolchain container reuse.
- [ ] **[OPEN]** How firmwares consume it (see §1).
- [ ] **[OPEN]** Cross-device build matrix.

---

## Open questions needing Eric, not analysis

1. What systems are actually on your card - Brick and Miniloong?
2. What do the controls physically allow on each device?
3. Same curation on both devices, or different per device?
4. Is integer-scale-only a principle, or a Brick luxury?
