# 0023. Diatom is MIT and ships no cores, which is what makes that safe

- **Status:** Proposed
- **Date:** 2026-08-26
- **Supersedes:** -
- **Superseded by:** -

Settles the §1 register item open since 2026-08-23. Relates to
[ADR-0002](0002-separate-repository.md) and [ADR-0006](0006-resident-core-model.md).

**Not legal advice.** This is reasoning from primary sources, held to the same
standard as any other decision here. Anyone shipping a product should get their
own advice; what this does is stop the question being re-argued from memory.

## Context

The README states Diatom's purpose as *"a permissively licensed replacement for
the GPL minarch that PlayOS currently patches and rebuilds."* That premise rests
on an unexamined claim: that an MIT frontend may load GPL cores. The register
noted the enabling fact in one line - `libretro.h` is itself permissive - and
left it there for three days.

**What the cores actually are**, fetched from libretro's core-info repo on
2026-08-26 and now recorded by `tools/fetch-cores.sh` with the binaries:

| Core | Systems | Licence |
|---|---|---|
| `fceumm` | NES | **GPLv2** |
| `mednafen_pce_fast` | PC Engine | **GPLv2** |
| `mgba` | GBA, Game Boy, Game Boy Color | MPL 2.0 |
| `snes9x2010` | SNES | **Non-commercial** |
| `genesis_plus_gx` | Genesis, Master System, Game Gear | **Non-commercial** |

**Two of five are GPL, and GPL turns out to be the easy half.**

## The question, asked the right way round

"Are the cores covered if we use MIT" inverts it. A licence attaches to the work
it is on; Diatom's MIT licence governs Diatom's code and cannot reach a core.
The question that matters is the reverse: **does loading a GPL core place
obligations on Diatom?**

Three facts answer it.

**Diatom ships no cores.** `CORES.md` says so and the binaries are gitignored.
The repository contains no GPL code, so distributing Diatom alone under MIT
raises nothing. This is not a technicality - it is the structural reason the
rest holds, and it is worth not quietly abandoning later for packaging
convenience.

**`libretro.h` is MIT and says so in a scoped notice**: *"The following license
statement only applies to this libretro API header."* The interface is
deliberately permissive so that any-licence frontends can host any-licence
cores. That is the enabling fact the register named.

**MIT is GPL-compatible, and this is what actually settles it.** Take the
strictest reading available - the FSF's, that a dynamically loaded plugin
sharing data structures with its host forms one combined work. Even then there
is no *conflict*, because MIT's conditions are a subset of what the GPL permits.
A combined distribution of Diatom and a GPLv2 core can be conveyed under GPLv2,
while Diatom's own source remains MIT for everyone else.

The outcome people fear - a GPL core "infecting" the frontend and forcing a
relicence - does not arise. What does arise is an obligation on **whoever ships
the image** to comply with GPLv2 for the GPL parts, including offering source.
That falls on the firmware distributor, not on this repository.

## Decision

**Diatom stays MIT, ships no cores, and states the reasoning here so it is not
re-derived.**

Three rules follow, and the third is the one with teeth:

1. **No core binary enters this repository**, and none is fetched at build time
   into anything Diatom distributes. `tools/fetch-cores.sh` writes to a
   gitignored directory for testing only.
2. **`libretro.h` keeps its own scoped notice**, and `LICENSE` names it
   separately. It already does.
3. **Core licences are recorded by the tool that fetches the cores**, not by
   anyone's memory. `fetch-cores.sh` now reads the `license` field from
   libretro's core-info repo and writes it into `CORES.md` beside the hash. The
   question this ADR answers sat open for days while the answer was three
   commands away.

## Consequences

**The non-commercial cores are the real exposure, and the register never
mentioned them.** `snes9x2010` and `genesis_plus_gx` are "Non-commercial":

- not open source under either the OSI or FSF definition;
- **not GPL-compatible**, so they cannot go into a GPL combined work at all;
- restricted against commercial redistribution outright.

Between them they cover **SNES, Genesis, Master System and Game Gear - four of
the nine systems the README advertises.** For a hobby firmware this is likely
tolerable and is what other firmwares already do. For anything commercial it is
not, and it constrains the product Diatom was written to enable far more than
the GPL question it was asked about.

This is not Diatom's problem to solve - it ships no cores - but a launcher
author choosing a core set inherits it, and until now nothing here said so.

**Easier.** The premise in the README is now supported rather than assumed. A
launcher author can see at a glance which cores restrict what.

**Harder.** Rule 1 forecloses a convenience: Diatom can never ship a "batteries
included" bundle without reopening all of this. That is a real cost and it is
accepted deliberately, because the alternative is a licence question in the
build system rather than in a document.

**Foreclosed.** Nothing about the code. This constrains distribution, not
design.

## Revisit if

- Diatom is ever proposed to ship, bundle or auto-download a core, which
  converts this from a settled question into an open one on the same day.
- A core's licence changes, or the pinned set gains one whose licence is neither
  permissive, GPL-compatible, nor merely non-commercial - a no-derivatives or
  no-redistribution core would break the "ships no cores" reasoning by making
  even the user's own copy questionable.
- Anyone builds a commercial product on this, at which point the two
  non-commercial cores need replacing and the systems they cover need a
  different answer.
