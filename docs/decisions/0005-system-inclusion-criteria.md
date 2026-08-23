# 0005. System inclusion criteria

- **Status:** Accepted
- **Date:** 2026-08-23
- **Supersedes:** —
- **Superseded by:** —

## Context

Diatom needs a rule for what it targets. Without one, every candidate system is
relitigated from scratch and the memory envelope never settles.

The working framing had been *"an 8-bit to 16-bit home console system."* That
describes the intent but **fails as a rule** — two confirmed inclusions break it:

- **GBA is 6th generation** (2001, 32-bit ARM) — one generation *later* than the
  PlayStation being excluded, and the same generation as Dreamcast and PS2. It is
  in because Metroid Fusion is a must-have.
- **PC Engine** is an 8-bit CPU (HuC6280) with 16-bit video, marketed as 16-bit.
- It is also a *handheld*, not a home console, as are Game Boy and Game Gear.

Meanwhile **Neo Geo AES is 4th-generation, 16-bit, 2D and digital** — squarely
inside the stated framing — and is excluded, for memory weight alone.

So generation predicts the decisions wrongly in both directions. A rule that
matches the actual reasoning is needed.

Relevant prior decisions: ADR-0003 (digital-only input) already excludes
analog-dependent libraries independently of this.

## Options considered

### Option A — keep the generational framing
"8-bit to 16-bit home consoles." Simple to say, but already contradicted by GBA
and PC Engine, and it admits Neo Geo, which is out. A rule that needs two
exceptions on day one is not a rule.

### Option B — an explicit system list, no criteria
Just enumerate. Honest, but provides nothing when a new candidate appears —
every future system becomes a fresh argument with no precedent to apply.

### Option C — criteria derived from the decisions actually made
Reverse-engineer the rule from the choices. More work up front; predicts future
cases.

## Decision

**Option C.** A system is in scope for Diatom if it satisfies **all four**:

1. **2D — sprite and tile based.** Excludes PS1, Saturn, N64.
2. **Digital input only.** Per ADR-0003. Excludes the analog PS1 library.
3. **Light enough that its core can stay resident** alongside every other core.
   Excludes Neo Geo (100+ MB ROMs).
4. **Integer-scales cleanly on a 4:3 panel.** Satisfied by everything above,
   on both known devices (Brick 1024×768, Miniloong 960×720 effective).

These four predict every decision made to date; the generational framing predicts
two of them wrong.

**Storage medium is explicitly not a criterion.** PC Engine CD is a PC Engine —
same CPU, same video chip, same core (`mednafen_pce_fast`), with a storage
peripheral attached. The HuCard/CD split is a *media* distinction, not a
generational one. PS1 is excluded on rules 1, 2 and 3 — not because it uses
discs.

Resulting list at time of writing:

| Status | Systems |
|---|---|
| **In** | NES · PC Engine (HuCard) · PC Engine CD · Genesis · SNES · GBA |
| **Out** | Neo Geo (rule 3) · PS1 / Saturn / N64 (rules 1, 2, 3) |
| **Unasked** | Master System · Game Gear · Game Boy / GBC · Lynx · Sega CD · Atari 7800 — all pass the four rules trivially; simply not yet decided |

## Consequences

**Easier:** New candidates get tested against four rules instead of argued from
scratch. The memory envelope follows directly — peak loaded game roughly 10–30 MB
(estimated), which is what makes ADR-0006 possible.

**Also easier than expected:** systems are not the cost unit — *cores* are.
`genesis_plus_gx` covers Genesis, Master System, Game Gear and Sega CD in one
binary; `mednafen_pce_fast` covers HuCard and CD; `gambatte` covers GB and GBC.
Six cores cover nine systems, so several of the unasked systems cost nothing.

**Harder:** The rules are stricter than taste. Any future system that fails one
requires either an explicit exception or superseding this ADR — which is the
intent.

**Known imperfection:** GBA satisfies all four rules but is a 6th-generation
32-bit handheld. The rules are about *shape*, not era, and that is deliberate —
but the project's informal description as "8-bit and 16-bit" will keep being
approximately true rather than exactly true.

**Not settled here:** PC Engine CD needs a System Card BIOS and a sane
"BIOS missing" error path, and multi-disc titles would need libretro's Disk
Control Interface. SNES in-cart coprocessors (SuperFX, SA-1, DSP-1, CX4) cost CPU
rather than RAM and may not reach full speed on Cortex-A53 — Star Fox and
Yoshi's Island are the usual casualties. This affects core selection
(`snes9x` vs `snes9x2010`/`2005`), which is a test-matrix question, not a
scoping one.

## Revisit if

- A wanted system fails exactly one rule — that is the signal to examine whether
  the rule is right, rather than to quietly grant an exception; or
- the target device class (§0b) changes such that rule 3 or 4 no longer binds.
