# 0008. Diatom runs as a separate, long-lived process

- **Status:** Accepted
- **Date:** 2026-08-23
- **Supersedes:** —
- **Superseded by:** —

## Context

PlayOS today runs `minarch.elf` as a child process, talking to it over a fifo.
Its documentation gives the reason as a GPL firewall: minarch is GPL-3.0, the
launcher is 0BSD, and keeping them separate programs preserved that boundary.

**Diatom is 0BSD, so that reason is gone.** The question of whether to keep the
process split has to be re-argued on its own merits rather than inherited.

Facts bearing on it:

- Cores are third-party C emulators. A segfault in one is a realistic event, not
  a hypothetical.
- **PlayOS's boot hook powers the device off if the launch loop exits.** A crash
  that takes down the launcher does not produce an error screen; it produces a
  device that switches itself off.
- [ADR-0006](0006-keep-all-cores-resident.md) has Diatom hold every core mapped
  for its lifetime, so whatever Diatom is, it wants to be long-lived rather than
  spawned per game.
- [ADR-0002](0002-separate-repository.md) intends Diatom to serve EROS,
  Contrarian and future firmwares, not PlayOS alone.
- The architecture is **already proven on this hardware** — resident minarch as a
  child process, cores held between games, display handed back and forth, works
  on tg5040 today.

## Options considered

### Option A — link Diatom into the launcher as a library
No IPC, no protocol to design or maintain, one process, slightly less RAM. But a
core segfault takes the launcher with it, which on PlayOS means the device powers
off. It also raises the bar for other firmwares: adopting a library means
matching build systems, toolchains and headers, where adopting a binary means
running it.

### Option B — a separate process spawned per game
Maximum isolation, and process exit guarantees memory reclaim. Incompatible with
ADR-0006 — every launch would re-pay `dlopen` for every core, roughly 170 ms
each, which is the cost that resident mode exists to avoid.

### Option C — a separate, long-lived process
Cores mapped once and held. Crash isolation intact. Lower coupling for other
firmwares. Matches what already works in PlayOS.

## Decision

**Option C.** Diatom is a separate process with a lifetime spanning many games.
The launcher communicates with it over a protocol rather than a function call.

## Consequences

**Easier:** A core crash kills Diatom, not the launcher — so PlayOS can show an
error and restart it, rather than the boot hook powering the device off.
Restarting costs re-mapping the cores, which is recoverable. Other firmwares
integrate against a binary and a protocol instead of a build system. And process
exit remains a guaranteed reclaim path if a core ever does leak badly enough to
matter.

**Harder:** Two processes contend for the display. On fbdev or DRM only one can
hold it at a time, and handing it over is genuinely fiddly. This is a real cost,
mitigated by PlayOS already doing it successfully on tg5040 — but it will need
solving again for each new port, and it is the most likely source of
platform-specific pain.

There is also a protocol to design and maintain, which a library would not need.

**Newly open, and larger than this decision:** the protocol itself. PlayOS uses
a line-based fifo today. Whether Diatom inherits that or specifies something
better is unresolved — and it matters more than this ADR, because it is the
interface other firmwares are stuck with. Tracked in §3 of the register.

**Deliberately not decided here:** how firmwares *obtain* Diatom — submodule,
subtree, vendored copy, or prebuilt artifact. Deferred, because nothing consumes
Diatom yet. See the register §1 note.

## Revisit if

- Display handoff proves unsolvable on a target port — that would force the
  library model despite its costs; or
- the protocol grows so chatty that IPC shows up in profiles; or
- Diatom still has exactly one consumer when it reaches parity with minarch, at
  which point the coupling argument has not paid off.
