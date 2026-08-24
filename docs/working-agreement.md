# Working agreement

How this project is run. Half of it is about process rather than about the code,
and that is deliberate: the goal is a frontend worth building *and* a project run
sustainably. Jumping straight to code is the specific habit being corrected, so
it is governed by rule rather than left to willpower.

---

## Phases

### Phase 1 - Scoping · CLOSED 2026-08-23

Nine ADRs, zero lines of implementation. All exit criteria met:

1. Every `[LB]` item resolved by an ADR or deferred *with a written reason*.
2. Name chosen and collision-checked ([ADR-0004](decisions/0004-name-the-project-diatom.md)).
3. Repo layout decided; consumption model deliberately `[DEFERRED]` with a reason
   and a revisit trigger, because nothing consumes Diatom yet.
4. Test matrix fixed: ten systems, five or six cores.
5. ~~The `dlclose` memory-reclaim question has a measured answer.~~
   **Retired** by [ADR-0006](decisions/0006-keep-all-cores-resident.md): Diatom
   never calls `dlclose`, so the question is moot. Retired by a later decision,
   not skipped.

See [the scoping close](discussion/2026-08-23-scoping-close.md).

### Phase 2 - Spikes · CLOSED 2026-08-23

Three empirical questions that argument could not settle.

1. **Environment-call inventory** ([result](spikes/2026-08-23-env-inventory.md)).
   All six cores, real ROMs, full lifecycle. 34 of 77 commands appear, ~17 must
   be implemented, 17 are declined by every core with nothing breaking.
2. **`nm -D` symbol check.** ADR-0006's revisit trigger fired. Five of six cores
   export only `retro_*`; PicoDrive exports 1069 others including a whole static
   zlib. The decision stood but `RTLD_LOCAL` became the *only* protection rather
   than a redundant one, hence
   [ADR-0010](decisions/0010-rtld-local-is-mandatory.md).
3. **Brick measurements** ([result](spikes/2026-08-23-rss-and-dlopen.md)).
   Six cores mapped plus one running is 15.0 MB, against a 250 MB trigger and
   975 MB of RAM. The estimate had been wrong by 10x, in the safe direction.

   *Marking this optional was a mistake.* An ADR whose revisit trigger cannot be
   evaluated is a decision resting on a guess. It should have been required, and
   only the absence of a device to hand made it look optional.

### Phase 3 - Implementation · CURRENT

Desktop backend first ([ADR-0007](decisions/0007-port-interface.md)). The port
seam is only honest with two real implementations behind it.

**Do not start a phase before the previous one's criteria are met.** If the
criteria start to feel like an obstacle, argue about them explicitly rather than
quietly skipping them.

---

## Practices

1. **Discussion is not decision.** Nothing is decided until an ADR exists and
   says `Accepted`. Never summarise an open question as though it were settled,
   and never let an unstated assumption harden into a premise.
2. **Every `[LB]` decision gets an ADR** before anything depends on it.
3. **Push back rather than validate.** If an idea is weak, say which part and
   why. Agreement given by reflex is worthless; the value is friction applied at
   the right places.
4. **Verify before asserting.** Read the actual file, manifest or spec rather
   than recalling it. Anything unverified must be labelled as unverified. This
   is written down because it was violated twice on the project's first day: an
   emulation requirement that turned out not to exist, and a feature proposed
   for a platform that does not support it.
5. **Update the register in the same session** as the discussion that changed
   it. A stale register is worse than no register, because it still gets
   trusted.
6. **Record why, not just what.** When introducing a convention, say what it is,
   why it exists, and what it costs.
7. **No implementation code outside the current phase.** Illustrative snippets
   to make a design point are fine. Scaffolding and "just a quick prototype" are
   not.

   **Exception - spikes.** Some exit criteria require a measured answer, which
   requires running something. A **spike** is a time-boxed throwaway that answers
   exactly one written question. It must state its question up front, live
   outside the repository, record its result in the register, and be deleted or
   archived once answered. It may never become the implementation. The failure
   mode this guards against is a prototype quietly turning into the codebase,
   which is how "I'll clean it up later" projects start.
8. **Apply the seam test** (register §0) to every proposed abstraction, and the
   same scepticism to process. Ceremony that does not earn its keep gets cut.
9. **Flag assumptions explicitly** rather than burying them in prose.
10. **One dated log entry per working session** in `discussion/`, written at the
    end, capturing what was concluded and what remains open.

---

## How decisions are made

```
question surfaces  ->  logged in scoping-register.md as [OPEN]
      |
discussed          ->  captured in discussion/YYYY-MM-DD-*.md
      |
decided            ->  decisions/NNNN-short-title.md  (an ADR)
      |
register updated   ->  item ticked, linked to the ADR
```

An **ADR** (Architecture Decision Record) is a short document capturing one
decision: the context, the options considered, what was chosen, and the
consequences. They exist because the expensive thing to lose is not *what* was
decided, it is *why*. Six months on, the reasoning is what distinguishes a
still-valid decision from one whose premises have expired.

ADRs are **immutable once Accepted**. Changed your mind? Write a new one that
supersedes the old, and mark the old `Superseded by NNNN`. Never edit the
reasoning of an accepted record; the fact of having believed it is itself the
useful history.

---

## What Diatom is - read before writing anything

**Diatom is a minimal libretro frontend.** It loads a core, runs a game, and puts
pixels and sound where the device wants them. That is the whole scope.

**Diatom has no core list and no system list.** It loads whatever core it is
handed. Any core or system named anywhere in this repository is a **test-matrix
entry**, the set Diatom has been verified against, never a capability and never a
dependency. Which core covers which system is the *host application's* config
decision (in PlayOS's case, `systems.cfg`).

**PlayOS is the first consumer, not the definition.** It is being built at the
same time, so it adapts to Diatom rather than the reverse. Its current
implementation, with a fifo protocol, a resume-slot temp file and a boot hook
that powers the device off, is one firmware's state of play and not a constraint
Diatom inherits. Other consumers may follow.

**Diatom is not a minarch fork, a NextUI component, or a MinUI derivative.** It
shares an API with them, libretro, and nothing else. What NextUI ships, which
cores MinUI bundles, and how minarch is structured are all irrelevant to what
Diatom can do. This error recurred three times, so it is written down.

> Evidence from other projects is welcome: measured hardware numbers, `dlopen`
> costs, what a working system proves is possible. Their *choices* are not
> evidence of anything.

---

## Code conventions

*Conventions, not decisions. They are pervasive but cheap to reverse, so they
live here rather than in an ADR (see [the ADR bar](decisions/README.md)).*

- **Symbol prefix is `diatom_`**, not `dia_`. Two reasons. Explicitness: an
  abbreviation that saves three characters and costs a moment's recognition is a
  bad trade, and nothing here is typing-bound. Greppability: `media_` *contains*
  `dia_`, so `grep dia_` would hit every media path and type in the tree, the
  same substring trap that disqualified the name "Stem" (which hides inside
  "sy**stem**").
- **Prefer the explicit form generally.** Abbreviate only where the short form is
  the name people actually use (`fmt`, `ptr`, `len` in local scope). Never
  abbreviate a public identifier to save characters.
- **No em dashes** anywhere: comments, docs, commit messages. A single hyphen
  where a dash is genuinely needed, otherwise restructure.
- **The port must never include `libretro.h`**
  ([ADR-0007](decisions/0007-port-interface.md)). Checkable mechanically with
  `make check-seam`, and it is the test for whether the seam holds.
- **Cores are opened `RTLD_NOW | RTLD_LOCAL`. Never `RTLD_GLOBAL`, anywhere**
  ([ADR-0010](decisions/0010-rtld-local-is-mandatory.md)). This is the only thing
  preventing symbol collision between resident cores. Measured: PicoDrive exports
  1069 non-`retro_*` symbols including a whole static zlib. The failure mode is
  silent: wrong function, no error, presenting as an emulation bug.
- **No domain nouns in the port interface**: no `game`, `save`, `core`, `menu`.
  If a port function's name contains one, it is in the wrong layer.

## Document conventions

- **Dates are absolute and ISO** (`2026-08-22`), never "last week" or "recently".
- **Register tags**: `[LB]` load-bearing · `[OPEN]` needs a decision ·
  `[LATER]` safe to defer · `[DEFERRED]` was load-bearing, postponed with a
  reason and a trigger.
- **ADR filenames**: `NNNN-kebab-case-title.md`, numbered sequentially, never
  renumbered.
- **ADR statuses**: `Proposed` -> `Accepted` -> `Superseded by NNNN` /
  `Deprecated`.

---

## Terminology hazard

**"Core" is overloaded and will cause confusion if left alone.**

In libretro, a *core* is the emulator plugin (`mgba_libretro.so`). Diatom is not
a core; it is the **host**, the thing that loads and runs cores. The scoping
folder was originally named `TEMP_DISCUSSION_ABOUT_NEW_CORE_SYSTEM`, which read
as though the project were about building cores. ADR-0004 picked a name that does
not compound the ambiguity.

Never use "core" for Diatom's own internals. Say "host", "runtime", or the
specific module name. See [the glossary](glossary.md).
