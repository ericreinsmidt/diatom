# 0004. Name the project Diatom

- **Status:** Accepted
- **Date:** 2026-08-23
- **Supersedes:** —
- **Superseded by:** —

## Context

The host (see §2 vocabulary) needed a name before a repository could be created
under ADR-0002. Constraints that emerged during naming:

- **Collision filter, as set by Eric:** only collisions with *Linux gaming
  handheld CFWs and frontends* matter. Unrelated software sharing the name is
  acceptable.
- **Greppability matters more than discoverability.** Nobody else is expected to
  use this, so being findable by strangers is worthless — but being able to
  search one's own code, logs, and issues is not.
- **It must read as a component, not a firmware.** ADR-0002 makes this a
  separate project serving PlayOS, EROS, Contrarian and future firmwares. A name
  pairing with one consumer implies ownership by it. Precedent already exists in
  this stack: *minarch* is to NextUI what this is to PlayOS, and minarch does not
  rhyme with NextUI. Avoid an `OS` suffix.
- **Plant names are a saturated genre marker** in this space — OnionOS,
  GarlicOS, spruceOS, Allium, Koriki, and Eric's own Leaf.

Surveyed CFW landscape (2026-08-23): ArkOS, GammaOS, KNULLI, muOS, ROCKNIX,
OnionOS, MinUI, NextUI, PakUI, spruceOS, AmberELEC, GarlicOS, JELOS, Koriki,
Lakka, Allium, Batocera. Naming patterns are plants, the `-ELEC` family, the
`-UI` suffix, and `-OS`. **No stone or micro-scale names exist in the space.**

## Options considered

Explored by axis rather than by word. Rejected, with cause:

| Name | Axis | Why rejected |
|---|---|---|
| Sprig | botanical | Hack Club's Sprig is an open-source **handheld game console** — in-domain |
| Stem | botanical | "sy**stem**" contains it. In a libretro codebase `grep -ri stem` returns everything |
| Minim | musical | Collides in-namespace with **MinUI** and **minarch** — the exact lineage being left |
| Sustain, Quaver | musical | Viable; less on-thesis than the finalists |
| Cairn | stone | Widely associated with burial monuments |
| Scree | stone | Connotes unstable footing — a poor metaphor for a foundation |
| Pico | scale | **PICO-8 is in-domain** (the Miniloong's own ROM tree has a `PICO8` folder); Raspberry Pi Pico owns the embedded side |
| Pumice, Flint, Cobble, Chert | stone | All viable finalists |
| Jig, Chase, Escapement, Collet | workholding / mechanism | Strong metaphors — Jig is the most literally accurate description of the job |
| Cheepnis | Zappa house style | Fits the org name (Utility-Muffin-Research-Kitchen is Zappa's studio); descriptive value nil by design |
| Mote, Mite, Iota, Tittle, Micron | smallness | All viable |

## Decision

**Diatom.**

A diatom is a single-celled alga with a silica shell, individually invisible.
Chosen because:

1. **It means vanishingly small**, which is the project's actual thesis — a
   minimal libretro host inside ≤1GB with no swap, defined as much by what it
   deletes as by what it implements.
2. **It preserves the stone thread by zooming into it.** Chert — a finalist —
   forms from the accumulated silica skeletons of diatoms and radiolarians. The
   tiny thing and the rock are the same material at different scales.
3. **No collision in the filtered domain.** Verified 2026-08-23: no CFW, no
   libretro frontend, no handheld firmware of this name.
4. **Greppable.** Not a substring of anything common in this problem space —
   specifically unlike "stem".
5. **Reads as a component.** No `OS` suffix, a different register from PlayOS.

**Known out-of-domain collision, accepted:** `diatom-lang/diatom`, a programming
language on GitHub. Outside the stated filter. Recorded so it is not rediscovered
as a surprise.

## Consequences

**Easier:** ADR-0002's repository can now be created. Binary is `diatom.elf`.
The folder `TEMP_DISCUSSION_ABOUT_NEW_CORE_SYSTEM/` can be renamed and its
contents migrated, which CLAUDE.md lists as part of leaving the scoping phase.

**Harder:** Nothing material. Renaming later costs a repo rename and a
find-and-replace.

**Newly open:**
- **Symbol prefix.** Cores export `retro_*`; ours needs its own prefix so the
  two are never confusable when reading a stack trace or an `nm` dump.
  `dia_` or `diatom_` — unresolved.
- **Port naming.** The stone scheme (`flint` / `slate` / `chalk` for
  tg5040 / miniloong / desktop) was proposed when the project itself was a
  stone. It still works — diatoms become chert becomes rock — but it is no
  longer automatic. Unresolved.

## Revisit if

A CFW or libretro frontend named Diatom appears, or the name proves confusing in
practice with `diatom-lang`.
