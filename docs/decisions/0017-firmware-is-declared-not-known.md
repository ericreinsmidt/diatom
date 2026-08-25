# 0017. Firmware requirements are declared by the launcher, not known by Diatom

- **Status:** Accepted
- **Date:** 2026-08-25
- **Supersedes:** -
- **Superseded by:** -

## Context

PC Engine CD is in scope ([core selection](../reference/core-selection.md)) and
needs a System Card BIOS in the system directory. Everything else about it was
already settled: the Disk Control Interface is unnecessary (502 Redump entries,
zero multi-disc titles), and Beetle PCE Fast covers HuCard and CD from one
binary. This was the last piece.

Measured on the Brick, 2026-08-25, Beetle PCE Fast v1.31.0.0 against
*Ys Book I & II (USA).chd*:

| Case | Result |
|---|---|
| CD content, System Card present | runs - **59.81 fps against a 59.8200 target, 0 resyncs**, CHD read natively |
| CD content, System Card absent | `retro_load_game` returns **false** |
| HuCard content, System Card absent | runs normally |

Two facts follow from that table, and they are what force the decision.

**The BIOS requirement is per-content, not per-core.** The same binary needs a
System Card for a `.chd` and needs nothing for a `.pce`. So it cannot be
answered by knowing which core is loaded.

**The core tells the frontend nothing.** `retro_load_game` returning false is
the identical answer it gives for a corrupt ROM, an unsupported format, or a
file that is not there. libretro has no "what firmware do you need" query -
this is a real gap in the API, not something being overlooked. The core's *log*
does name the file it looked for (`MDFN_MakeFName: <dir>/syscard3.pce`), which
is the only signal available and is not part of any contract.

That matters more than a nicer error message, because of the rule in ADR-0009:

> `ERROR` means the game never started. `EXIT` means it ran and stopped.

The split is about **display ownership**. A missing System Card means the game
never started, so the launcher must keep drawing and be told `ERROR`. Reporting
it as `rom_unreadable` is not merely vague - it sends the player looking for a
bad download when the actual fix is a missing file they can supply.

Also measured, and it constrains what any solution can promise: a **2 KB file
of random bytes named `syscard3.pce` was accepted and the game reported
RUNNING.** The real card is 262,144 bytes. The core does not validate the
System Card at all.

## Options considered

### Option A - Diatom holds a table of core to firmware

What RetroArch does: a frontend-side database of required BIOS files, keyed by
core, with known-good hashes.

It would work, and it would validate rather than merely check presence. It also
contradicts a standing property of this project - *"Diatom has no core list. It
loads whatever core it is handed"* - and a firmware table is a core list under
another name. Worse, the table would be **wrong by construction**: the
requirement is per-content, so an entry for `mednafen_pce_fast` would demand a
System Card for HuCard games that do not need one. To be right it would have to
key on content type as well, which is a second database.

It also creates a maintenance obligation with no owner. Every new core, every
new system, every regional BIOS variant is an edit to Diatom.

### Option B - parse the core's log

The core prints the path it tried. Watch for it, check whether that file exists,
report `bios_missing` naming it.

Requires no protocol change and needs nothing from the launcher. It is also
coupling to a string in someone else's log output, which is not an interface and
will not survive a core update anyone else makes. Cheap now, silently broken
later, and broken in a way nothing would catch.

### Option C - infer from the file extension

`.chd` / `.cue` / `.iso` means CD, which means a System Card is needed.

Compact, but it is the same domain knowledge as option A wearing a smaller
coat, and it is not even true in general - Sega CD, PC-FD and Neo Geo CD are
all `.chd` with different firmware.

### Option D - the launcher declares it

The launcher says what this content needs: `RUN core= rom= firmware=syscard3.pce`,
and `--firmware` standalone. Diatom checks the named file is readable in the
system directory **before** asking the core to load anything, and fails with
`ERROR code=bios_missing msg=<name>` if it is not.

The launcher already maps system to core - PlayOS does it in `systems.cfg` - so
it is already the component that holds this class of knowledge, and firmware is
part of the same mapping. It generalises without further work: Sega CD, Neo Geo
and anything else needing firmware is the same field with a different value.

It costs a protocol key, which ADR-0009 makes nearly free by specifying that
unknown keys are ignored. An older Diatom drops `firmware=` and fails exactly as
it did before; a newer one checks.

## Decision

**Option D.** Firmware is named by whoever launches Diatom, and checked before
the core is asked for anything.

Diatom does not know that a PC Engine CD needs `syscard3.pce`, and will not
learn.

## Consequences

**Easier.** A missing System Card is now a distinct, actionable failure that
names the file: `ERROR code=bios_missing msg=syscard3.pce`, measured at 0.01 s
because the core is never loaded. `bios_missing` was specified in ADR-0009 and
emitted nowhere; it is real now. Any future firmware-needing system is
configuration rather than code.

**Harder.** The launcher must know which content needs what. That is a genuine
transfer of work, not a removal of it - the knowledge did not disappear, it
moved to the component already holding its siblings.

**The check proves presence, not validity**, and this is the honest limit of it.
A wrong, truncated or corrupt System Card passes: measured, 2 KB of random bytes
was accepted and the game reported RUNNING. Diatom will not close that gap,
because validating means knowing what the file should be, which is option A
again. The launcher can, and is better placed to - it already named the file, so
it can hash it too.

**A launcher that says nothing keeps the old behaviour**, which is deliberate.
Not sending `firmware=` yields `rom_unreadable` exactly as before. The failure
is vaguer, never wrong.

**Not a change to ADR-0009.** That ADR specified `bios_missing` as an error code
and specified that unknown keys are ignored. This uses both as designed.

## Revisit if

- libretro grows a real firmware query - an environment call a core can use to
  declare what it needs. Then the core is a better source than the launcher and
  this becomes a fallback.
- A single piece of content is measured to need firmware that varies by core -
  the same `.chd` wanting different files under two different cores. The field
  is content-scoped today; that observation would make it core-and-content
  scoped and the protocol would need to carry both.
- A launcher is observed shipping its own copy of a core-to-firmware table
  *and* getting it wrong in a way a Diatom-side table would have caught. That
  would be evidence the knowledge was put in the wrong place, and option A
  deserves re-argument on those facts rather than these.
