# Architecture Decision Records

One decision per file. `NNNN-kebab-case-title.md`, numbered sequentially,
never renumbered, never deleted.

## Why bother

The expensive thing to lose is not *what* you decided — you can read that off
the code. It's *why*. Six months later, the reasoning is the only thing that
lets you tell a decision that's still correct from one whose premises quietly
expired. Without it you get two failure modes, both common:

- **Cargo-culting** — preserving a constraint whose original reason is long
  gone, because nobody remembers whether it still matters.
- **Relitigating** — re-deciding the same question every few months because the
  arguments were never written down, only had.

An ADR costs ten minutes and kills both.

## When to write one

- Any register item tagged **[LB]** (load-bearing).
- Anything you'd be annoyed to have to re-argue.
- Anything where you rejected a reasonable alternative — *especially* then. The
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
record — the fact that you once believed it, and why, is itself the useful
history. Editing it destroys exactly the thing the practice exists to preserve.

## Template

Copy `template.md`. Keep it short — an ADR that takes an hour to write won't
get written next time, and a practice you abandon is worth less than one you
never started.

## Index

| # | Title | Status |
|---|---|---|
| [0001](0001-record-architecture-decisions.md) | Record architecture decisions | Accepted |
| [0002](0002-separate-repository.md) | Build the frontend in its own repository | Accepted |
| [0003](0003-digital-only-input.md) | Digital-only input — no analog support | Accepted |
| [0004](0004-name-the-project-diatom.md) | Name the project Diatom | Accepted |
| [0005](0005-system-inclusion-criteria.md) | System inclusion criteria | Accepted |
| [0006](0006-keep-all-cores-resident.md) | Keep all cores resident; never unload | Accepted |
| [0007](0007-port-interface.md) | The port interface | Accepted |
