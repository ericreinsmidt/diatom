# 0016. Saves, save states, and who owns the slot

- **Status:** Accepted
- **Date:** 2026-08-24
- **Supersedes:** -
- **Superseded by:** -

## Context

Diatom binds `retro_serialize` and never calls it, and does not bind
`retro_get_memory_data` at all. **Nothing persists.** Three play sessions
missed it only because Contra has no battery.

**Measured on the device, 2026-08-24**, across six cores and eight ROMs:

| Fact | Number | Why it matters |
|---|---|---|
| SRAM size | 0 to 128 KB, **per game not per system** | Probotector 0, Final Fantasy 8 KB, Golden Sun 128 KB. Query per game; write no file when zero |
| Save state size | 13 KB to 804 KB | SNES is the worst case |
| `retro_serialize` | 10 us to 2.5 ms | Free. The core is never the cost |
| Atomic write (write+fsync+rename) | **8 ms at 2 KB, 21 ms at 128 KB, 68 ms at 528 KB** | There is an ~8 ms floor on this filesystem regardless of size |
| `fsync` | no measurable cost | So it is always on |
| SIGTERM on power-off | **delivered, ~810 ms to 1.0 s before death** | The shutdown path is a real mitigation |

Two findings that constrain the implementation:

**`retro_serialize_size` is not stable and the quirk flags do not warn you.**
mGBA moved 528,448 to 462,912 within 600 frames of Golden Sun while reporting
`quirks 0x0`, having never set `RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE`.
Cache the size at load and the day it grows instead of shrinks, saving fails
silently.

**No save write can happen on the frame thread.** The budget is 16.6 ms and
`present` already costs 8.4 ms. Even a 2 KB write at 8 ms average and 14.6 ms
worst case blows it. This was assumed to be affordable and is not.

**`RETRO_MEMORY_RTC` is unused by our cores.** Boktai and Pokemon Emerald both
report `rtc 0` from mGBA. These are the RTC games; the API path is not a gap in
test coverage, it is simply not taken.

## Decision

### What persists

**SRAM is automatic and not optional.** It is the game's own data and losing it
is a defect, not a missing feature. Loaded after `retro_load_game`, written when
it changes, flushed on exit and on SIGTERM.

**Save states are opt-in**, because a state is a frontend convenience the game
knows nothing about.

**RTC is not implemented.** The persistence function takes a memory id and an
extension rather than hardcoding SRAM, so enabling it later is one line. Same
treatment ADR-0007 gave XRGB8888: accepted in shape, not built until something
needs it.

### Slots

**Five manual slots, plus one resume slot that is not one of them.**

The separation is not taste. If exiting wrote to a numbered slot it would
silently destroy a save the user made deliberately. Diatom owns the resume slot;
the user owns the five.

### When

| Event | SRAM | State |
|---|---|---|
| Game load | read | resume slot read, if the launcher asks |
| During play | write when changed, at most every 1 s, on a writer thread | manual only |
| MENU pressed | - | user's choice from the launcher menu |
| Clean exit | flush | resume slot written |
| SIGTERM | flush | resume slot written |

**Resume is the default on launch.** A launcher that wants a fresh start says
so; the common case needs no thought.

**"Quit without saving" is answered at launch, not at quit.** Because the resume
slot cannot clobber a manual save, exiting always writes it harmlessly, and the
real choice - resume, start fresh, or load slot 3 - is made when the player next
launches and knows what they want. No modal on the way out.

### Where

`<save_dir>/<rom>.srm`, `<rom>.state.auto`, `<rom>.state1` through `.state5`, as
**defaults for standalone use only**. The launcher passes explicit paths and
those defaults never apply to it.

### Who

**Diatom takes paths and never slot numbers.** It has no concept of a slot, a
game, or a system; the launcher composes the path and owns the menu (ADR-0009).

**The in-game menu is the launcher's, reached by MENU, with the display handed
over while the game is paused.** Diatom owns the input during play, so it detects
the press and reports it; it does not draw anything. Note that "pause" here means
the frontend menu, not the game's own pause, which stays on Start where the
system put it.

### How

- **Atomic writes always**: write `.tmp`, `fsync`, `rename`. A power cut must
  never corrupt an existing save. `fsync` measured free, so there is no reason
  to skip it.
- **A writer thread**, same shape as the port's flip thread. The frame loop
  snapshots and signals; it never blocks on a card.
- **`retro_serialize_size` is called immediately before every serialize**, never
  cached, because mGBA proves it moves without saying so.
- **State files carry a header**: core name, core version, payload size. A state
  written by a core that has since changed is refused with a message rather than
  fed to it.
- **A mismatched `.srm` loads `min(file, sram)` with a warning** rather than
  being refused. A partial load usually still yields a working save; refusing
  guarantees the player loses it.
- **The signal handler sets a flag and does no I/O.** The frame loop notices
  after `retro_run` returns, costing about 10 ms of the ~810 ms budget.

## Consequences

**Easier:** Progress stops being lost. Switching the device off mid-game becomes
a supported way to stop playing, which is how handhelds are actually used.

**Harder:** Diatom grows a thread and a file format. The state header is a
compatibility surface that will need versioning the first time it changes.

**Sequencing this exposes:** SRAM, resume-on-launch and state-on-exit need only
command-line flags and can ship immediately. Manual slots need the launcher menu,
which needs the ADR-0009 protocol *and* display handoff. So the data-loss defect
is fixable now and the slot feature is gated behind the hardest open item.

**Risk carried, stated plainly:** ADR-0013 chose raw fbdev for the Brick because
the mali EGL swap blocked two vblanks. PlayOS drives its own UI through EGL. A
handoff between an fbdev presenter and an EGL presenter is exactly the
combination that wedged the PowerVR firmware in-kernel this morning. PlayOS and
minarch already hand off successfully, but both are EGL, so their success is not
evidence for our case. **This must be spiked before the menu is designed.**

## Revisit if

- the display handoff proves unreliable, which would force manual saving back to
  blind hotkeys or an overlay Diatom draws itself; or
- five slots turns out to be the wrong number, which is a constant; or
- a core appears that uses `RETRO_MEMORY_RTC`, or writes its own save files into
  the save directory while also reporting SRAM, which would double-handle.
