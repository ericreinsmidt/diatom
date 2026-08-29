# 0025. Achievements belong to the frontend, and rcheevos is the evaluator

- **Status:** Proposed
- **Date:** 2026-08-29
- **Supersedes:** -
- **Superseded by:** -

Touches [ADR-0002](0002-separate-repository.md) (the permissive-licensing
goal, which decides *which* library may be vendored), [ADR-0007](0007-port-seam.md)
(what a port may see) and [ADR-0009](0009-launcher-protocol.md) (the launcher
drives; this adds messages in both directions).

## Context

TortOS wants RetroAchievements. The question was where the work goes, and the
first instinct - the launcher, because that is where the network and the UI
already are - is wrong for a reason that is measurable rather than aesthetic.

**Achievement conditions are evaluated against the previous frame.** A real
condition from Blaster Master's set, fetched 2026-08-29:

    R:0xH06f3!=0_R:0xH0014!=7_R:0xH040d<d0xH040d_O:d0xH047d=0_N:d0xH047b=1

`d0xH040d` is "that byte's value last frame". `R:` resets an in-progress
match, `N:`/`O:` chain conditions, and other forms carry hit counts that must
increment once per frame and no more.

**The launcher cannot see frames.** TortOS is awake during a game, but its
loop is `diatom_wait()`, polling the socket with a 100ms timeout: 10Hz against
a core running at 59.7-60.1Hz (measured, `docs/reference/core-facts.md`). Six
frames in seven are never observed. Delta and hit-count conditions would miss
unlocks silently and non-deterministically, which is the worst failure
available to a feature whose entire job is noticing things.

**The libretro API assigns this to the frontend explicitly.** From
`src/libretro.h`, on `RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS`:

> Explicitly notifies the frontend of whether this core supports achievements.
> The core must expose its emulated address space via `retro_get_memory_data`
> or `RETRO_ENVIRONMENT_GET_MEMORY_MAPS`.

libretro's own documentation is equally direct: the core exposes memory, and
"everything else should be handled by the libretro frontend". Diatom is the
frontend. TortOS never loads a core and never sees one.

**Diatom already receives both signals and discards them.** `src/env.c`
accepts `SET_MEMORY_MAPS` and `SET_SUPPORT_ACHIEVEMENTS` under
"accepted and ignored", and `src/core.c` already binds `get_memory_data` and
`get_memory_size` - `src/save.c` uses them for SRAM today. The plumbing for
this exists and is being dropped on the floor.

**Identification already works and needs none of this.** TortOS's
`tools/ra-check.py` implements RA's per-system hash rules and resolves 168 of
180 ROMs (measured 2026-08-29); 8 of the 12 misses are fan translations, which
are in no No-Intro-derived database under those names.

**Assumed, not measured:** that an unregistered client may still submit
unlocks. RA injects a `Warning: Unknown Emulator` achievement (condition
`1=1.300.`, true after 300 frames) into sets fetched by clients it does not
recognize, but the achievement data itself arrives complete and correctly
pointed - 27 achievements at 0/5/10/25/50 points. Whether `awardachievement`
is honored from an unregistered client was deliberately not tested, because
testing it writes to a real account.

## Options considered

### Option A - evaluate in the launcher, stream memory over the socket

Diatom answers memory reads; TortOS evaluates. Keeps Diatom small and keeps
every byte of RetroAchievements in one repository.

Costs: a 60Hz conversation on a unix socket carrying arbitrary reads, and it
does not work anyway - the launcher's loop is 10Hz, so the delta conditions
that most achievements are built from cannot be evaluated correctly. Making it
work means rewriting the launcher's wait loop into a frame-paced one, at which
point the launcher is a frontend with extra steps.

### Option B - Diatom evaluates, with its own condition implementation

No third-party runtime code, which has been true of this repository since
ADR-0002 and is worth something.

Costs: RetroAchievements owns the condition language and changes it. An
implementation here tracks someone else's spec forever, and the failure mode of
being subtly behind is an achievement that never fires - indistinguishable, from
the player's side, from one they have not earned. The language is not small:
deltas, priors, hit counts, alt groups, AndNext/OrNext chains, and measured
values.

### Option C - Diatom evaluates, vendoring rcheevos

rcheevos is RetroAchievements' own reference implementation, **MIT licensed**,
2.8MB of repository. Same license as Diatom, so ADR-0002's founding goal -
escaping GPL inheritance - is untouched. It is also what every other frontend
uses, including minarch, which this project replaces.

Costs: it is a second vendored dependency after `libretro.h`, with its own
build. It is also large enough that "self-contained" starts to mean something
different than it did.

## Decision

**Option C. Diatom owns achievements and vendors rcheevos.**

This is the same shape as `libretro.h`, already vendored: someone else's
definition of an interface that must be matched exactly, carried in so the
build stays self-contained. Reimplementing either would be re-deriving a
specification we do not own.

**The split with the launcher follows ADR-0009 unchanged: the launcher
drives.** TortOS logs in, hashes the ROM, fetches the achievement set, and
shows unlocks. Diatom evaluates. This means using rcheevos's lower-level
`rc_runtime`, not `rc_client`: `rc_client` manages the whole session including
its own HTTP, which would put network code inside the emulator and invert the
protocol relationship every other feature obeys.

**Diatom gains no network access.** That is the line this ADR draws, and it is
the one to check any future change against.

## Consequences

Easier: the condition language is correct by construction and stays correct as
RetroAchievements changes it. Memory access is already bound. The two
environment calls become useful instead of ignored.

Harder: Diatom acquires a substantial dependency, a build rule for it, and an
entry in `THIRD-PARTY.md`. `make check`'s seam test (ADR-0007) must be looked
at, since rcheevos is neither port nor frontend and must not become a way for
one to see the other.

Foreclosed: evaluating anywhere but in the frontend. Any future "just read
memory over the socket" proposal is answered by the 10Hz measurement above.

New protocol surface, in both directions - the launcher sends a condition set
at RUN, Diatom reports unlocks. That is an extension of ADR-0009 and wanted its
own ADR once the shape was known rather than a guess here; it is
[ADR-0026](0026-achievements-on-the-launcher-protocol.md). **The console id is
part of that surface and is not optional**: a RetroAchievements address is an offset
into a per-console space, so `0x06f3` means nothing until someone says NES.
Diatom does not guess it from the core, because the core does not know either.

One file of rcheevos is reimplemented rather than vendored, and that is worth
naming: `rc_libretro.c` turns libretro's memory descriptors into RA's address
space, and is excluded for the reason in the table above. `src/cheevos.c` does
that mapping instead, following upstream's algorithm rather than inventing one,
and says so at the top of the file. The console tables it reads are the
vendored ones, unmodified. Not vendoring a file is not a license to guess at
what it did.

`make check-seam` grew a second clause. rcheevos sits on the frontend's side of
ADR-0007's seam, so a port including any of its headers now fails the build the
same way including `libretro.h` does.

Not addressed: client registration with RetroAchievements. Unregistered
clients get a warning achievement injected into every set. That is Eric's
request to make and no code changes it.

## Revisit if

- RetroAchievements publishes a stable condition-language specification
  versioned independently of rcheevos, which would make Option B a matter of
  effort rather than of chasing a moving target.
- rcheevos's license changes from MIT, which would put it in conflict with
  ADR-0002 and require removing it rather than upgrading it.
- ~~The vendored source exceeds a size where "self-contained build" stops being
  honest - concretely, if rcheevos plus its build is larger than the rest of
  `src/` combined.~~

  **This fired on the first build that linked it**, 2026-08-29: 77,517 bytes of
  machine code against Diatom's own 42,548, and 11,529 source lines against
  4,634. rcheevos is about 1.8x the frontend it is going into.

  It does not change the decision, and the clause was written badly. Both
  alternatives are *larger*: Option B is a hand-written implementation of the
  condition language whose reference implementation is the 11,529 lines being
  weighed, and Option A does not work at 10Hz at any size. Nor is there
  anything to trim - each of the 18 compiled files was removed in turn and the
  link fails on every one, so this is the floor for using rcheevos at all
  (`vendor/rcheevos/README.md`).

  What the size actually bears on is §11's RSS budget, which is a device
  measurement and has **not** been re-run. That is the falsifiable form of this
  clause and replaces it: **revisit if the device RSS budget no longer holds**,
  rather than if a ratio crosses a line with no consequence attached.
