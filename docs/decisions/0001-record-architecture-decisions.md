# 0001. Record architecture decisions

- **Status:** Accepted
- **Date:** 2026-08-22
- **Accepted:** 2026-08-23
- **Supersedes:** -
- **Superseded by:** -

## Context

This project has two goals: build a libretro frontend, and practice running a
project sustainably. The stated problem being corrected is a habit of starting
projects and not managing them cleanly over time.

There is already concrete evidence of what that costs, from PlayOS on
2026-08-22. Its documentation states the minarch overrides are diffs against
NextUI `v6.11.2`. The build script actually reads `$HOME/Projects/NextUI/
workspace`, which on the mini sits at `v6.11.2-10-g96eeacd9` on a work-in-
progress branch. The documented baseline and the real one silently diverged, and
the divergence surfaced only because someone tried to reproduce the build on a
second machine. Nobody made a bad decision there - the decision just stopped
being written down anywhere that stayed true.

The frontend will involve a series of decisions that are expensive to reverse
and easy to forget the reasons for: the layer model, the process boundary,
residency policy, whether integer-only scaling is a principle.

## Options considered

### Option A - no formal record
Decisions live in conversation and in the code. Zero overhead. This is the
current default, and the PlayOS divergence above is what it produces at scale.

### Option B - a single running decisions log
One append-only file. Cheap, but entries get edited in place as thinking
evolves, so the history of *why* is progressively overwritten - losing the
main thing worth keeping.

### Option C - one file per decision (ADRs)
Numbered, immutable once accepted, superseded rather than edited. Slightly more
ceremony. Well-established practice, so the format is learnable rather than
invented here - which matters given professional development is an explicit
goal.

## Decision

Adopt ADRs (Option C) for this project. Every register item tagged `[LB]` gets
one before anything depends on it.

## Consequences

**Easier:** Revisiting a decision means reading one short file rather than
reconstructing an argument. Rejected options stay visible, so reconsidering
them is cheap. A second machine - or a reader six months out - can see intent,
not just outcome.

**Harder:** Every load-bearing decision now has a small tax before it counts as
made. That tax is the point, but it is a real cost and it is the reason this
practice usually fails.

**The actual risk:** abandonment. A process you stop following is worse than
one you never started, because the half-maintained record still gets trusted.
Mitigation is keeping ADRs genuinely short and writing them only for decisions
that meet the bar in `decisions/README.md`.

## Revisit if

Three or more `[LB]` decisions get made without ADRs - that's evidence the
ceremony is too heavy, and the format should be cut down rather than quietly
ignored.
