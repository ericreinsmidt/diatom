# Architecture Decision Records

One decision per file. `NNNN-kebab-case-title.md`, numbered sequentially,
never renumbered, never deleted.

## Why bother

The expensive thing to lose is not *what* you decided - you can read that off
the code. It's *why*. Six months later, the reasoning is the only thing that
lets you tell a decision that's still correct from one whose premises quietly
expired. Without it you get two failure modes, both common:

- **Cargo-culting** - preserving a constraint whose original reason is long
  gone, because nobody remembers whether it still matters.
- **Relitigating** - re-deciding the same question every few months because the
  arguments were never written down, only had.

An ADR costs ten minutes and kills both.

## When to write one

- Any register item tagged **[LB]** (load-bearing).
- Anything you'd be annoyed to have to re-argue.
- Anything where you rejected a reasonable alternative - *especially* then. The
  rejected option is most of the value.

Not for: reversible details, naming of internals, anything you could change in
an afternoon without consequence.

## Status lifecycle

```
Proposed  →  Accepted  →  Superseded by NNNN
                      ↘  Deprecated
```

**Accepted ADRs are immutable.** Changed your mind? Write a new ADR, and mark
the old one `Superseded by NNNN`. Never edit the reasoning of an accepted
record - the fact that you once believed it, and why, is itself the useful
history. Editing it destroys exactly the thing the practice exists to preserve.

## Template

Copy `template.md`. Keep it short - an ADR that takes an hour to write won't
get written next time, and a practice you abandon is worth less than one you
never started.

## Index

| # | Title | Status |
|---|---|---|
| [0001](0001-record-architecture-decisions.md) | Record architecture decisions | Accepted |
| [0002](0002-separate-repository.md) | Build the frontend in its own repository | Accepted |
| [0003](0003-digital-only-input.md) | Digital-only input - no analog support | Accepted |
| [0004](0004-name-the-project-diatom.md) | Name the project Diatom | Accepted |
| [0005](0005-system-inclusion-criteria.md) | System inclusion criteria | Accepted |
| [0006](0006-keep-all-cores-resident.md) | Keep all cores resident; never unload | Accepted |
| [0007](0007-port-interface.md) | The port interface | Accepted |
| [0008](0008-separate-long-lived-process.md) | Diatom runs as a separate, long-lived process | Accepted |
| [0009](0009-launcher-protocol.md) | The launcher ↔ Diatom protocol | Accepted |
| [0010](0010-rtld-local-is-mandatory.md) | `RTLD_LOCAL` is mandatory, and load-bearing | Accepted |
| [0011](0011-lock-the-display-rect.md) | Lock the display rect at load from base geometry | Superseded by 0021 |
| [0012](0012-independent-toolchain.md) | Build our own toolchain; depend on nothing from NextUI or MinUI | Accepted |
| [0013](0013-brick-fbdev-flip-thread.md) | The Brick presents via fbdev, with a flip thread | Accepted |
| [0014](0014-display-modes-and-default.md) | Offer six display modes; default to stretch | Accepted |
| [0015](0015-integer-vertical-mode.md) | Add integer-vertical; keep stretch as the default | Superseded by 0018 |
| [0016](0016-saves-and-save-states.md) | Saves, save states, and who owns the slot | Accepted |
| [0017](0017-firmware-is-declared-not-known.md) | Firmware requirements are declared by the launcher, not known by Diatom | Accepted |
| [0018](0018-integer-vertical-remeasured.md) | Integer-vertical, re-measured: it is not "never worse" | Accepted |
| [0019](0019-input-mapping-and-remapping.md) | Two input translations, and only one of them is remappable | Accepted |
| [0020](0020-shared-state-plane.md) | The protocol grows a state plane, with one owner per item | Accepted |
| [0021](0021-settle-the-rect-before-locking.md) | Lock the rect, but relock if what we locked onto was never real | Accepted |
| [0022](0022-display-mode-on-the-state-plane.md) | Display mode joins the state plane, and is the second contested row | Accepted |
| [0023](0023-core-licensing.md) | Diatom is MIT and ships no cores, which is what makes that safe | Proposed |
| [0024](0024-session-persistence-paths.md) | A session carries its persistence paths, and PREVIEW finally exists | Accepted |
| [0025](0025-achievements-belong-to-the-frontend.md) | Achievements belong to the frontend, and rcheevos is the evaluator | Proposed |
| [0026](0026-achievements-on-the-launcher-protocol.md) | Achievements on the launcher protocol: the console is declared and the set is a file | Proposed |
| [0027](0027-an-overlay-the-port-composites.md) | An overlay the port composites, because only one process can present | Proposed |
| [0028](0028-turbo-is-a-property-of-a-binding.md) | Turbo is a property of a binding, and the host owns it | Accepted |
| [0029](0029-audio-output-is-a-state.md) | Audio output is a state, owned by the launcher | Accepted |
