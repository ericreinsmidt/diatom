# 0006. Keep all cores resident; never unload

- **Status:** Accepted
- **Date:** 2026-08-23
- **Supersedes:** —
- **Superseded by:** —

## Context

Snappiness is a stated primary goal. PlayOS already demonstrates what is at
stake: holding cores between games took launch from **~1100 ms to ~200 ms**.

Costs, from PlayOS's own `ma_core.c` override note and from how memory works on
these devices:

- **`dlopen` is the expensive step — ~170 ms, once per core ever.** `Core_bind`
  (callbacks and paths, per game) is *microseconds*. So most of the ~900 ms saved
  came from keeping the **process and GL context** alive, not from holding
  multiple cores.
- A merely-mapped `.so` is **file-backed** — small, and evictable by the kernel
  under pressure. An initialized core's allocations are **anonymous**, and these
  devices have **no swap**, so anonymous memory cannot be reclaimed at all. Too
  much file-backed memory costs a page-in stutter; too much anonymous memory
  invokes the OOM killer.
- ADR-0005's envelope: peak loaded game roughly **10–30 MB** (estimated from ROM
  and system-RAM sizes, not measured). Six cores cover the nine candidate
  systems. Miniloong measured at **956 MB** total RAM.

An earlier proposal in discussion — *"never unload, keep exactly one core
resident, exit the process on system switch"* — was designed to bound memory
when Neo Geo (100+ MB ROMs) was still a candidate. **ADR-0005 excluded Neo Geo,
which removed the problem that proposal existed to solve.** It is not adopted.

## Options considered

### Option A — one core resident, exit process on system switch
Bounds memory at one core; process exit guarantees reclaim from leaky cores.
Solves a problem the ADR-0005 envelope no longer has, and pays ~170 ms plus a
process restart on every system switch — directly against the snappiness goal.

### Option B — resident with eviction (LRU or tiered)
Necessary only if cores could not all fit. They can, by roughly 6×. Requires
`dlclose`, which is a minefield: **static TLS can make a library permanently
unloadable while `dlclose` reports success**; cores assume one-shot process
lifetime and may leak across reloads; and glibc does not return freed heap to
the OS, so RSS is a misleading measure of whether it worked. All of that risk
for no benefit.

### Option C — `dlopen` every core, never `dlclose`, `retro_init` only the one in use
Pays the expensive step (`dlopen`) once per core and never again. Keeps only one
core's *state* allocated. `retro_init` is cheap, so initializing on demand costs
almost nothing.

## Decision

**Option C.** Every core is `dlopen`'d and never unloaded. `retro_init` runs only
for the core currently in use.

Concretely: `dlclose` is never called anywhere in Diatom.

## Consequences

**Easier:** System switching costs no `dlopen`. Only one core holds allocated
state, so anonymous memory stays near a single game's footprint. Residency
policy, eviction, LRU and tiering are all unnecessary. Crucially, **`dlclose`
never happens**, so static TLS, core reload leaks, and glibc's refusal to return
heap are all non-problems rather than hazards to design around.

**This retires a phase exit criterion.** CLAUDE.md required a measured answer on
whether `dlclose` reclaims memory. Diatom never performs that operation, so the
question is moot and the spike is not needed.

**Harder:** Mapped cores accumulate for the process lifetime. Cheap and
file-backed, so the kernel can evict code pages under pressure — the failure mode
is a page-in stutter, which for an emulator means a dropped frame. Not fatal, but
it is the thing to watch if the core list grows a lot.

**Foreclosed:** Any system heavy enough that its core cannot coexist with the
others. That is ADR-0005 rule 3, and the two decisions stand or fall together.

**Obligation this creates:** every core added must be checked with `nm -D` to
confirm it exports nothing but `retro_*` entry points, before it is trusted to
share an address space. PlayOS verified 45/45 fceumm, 53/53 mednafen_pce_fast,
25/25 mgba. `snes9x2010`, `genesis_plus_gx` and `gambatte` are **unverified**.
`RTLD_LOCAL` protects the entry points regardless — the `nm` check proves the
wider collision surface is empty, which is a stronger guarantee.

This is an **implementation precondition, not a scoping blocker**: the check runs
on the shipping tg5040 builds, on a workstation — `nm` inspects a binary and
needs no device. Do not run it against another device's builds; symbol exports
depend on source *and* build flags.

## Revisit if

- Measured RSS with all cores mapped and one initialized exceeds ~250 MB — a
  third of available RAM would mean the estimates were badly wrong; or
- any added core exports symbols other than `retro_*`; or
- ADR-0005 is superseded to admit a system that cannot coexist.
