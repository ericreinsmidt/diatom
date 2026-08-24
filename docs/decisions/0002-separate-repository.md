# 0002. Build the frontend in its own repository

- **Status:** Accepted
- **Date:** 2026-08-22
- **Accepted:** 2026-08-23
- **Supersedes:** -
- **Superseded by:** -

## Context

The frontend is intended to replace minarch in PlayOS and to serve as a base for
other firmware projects (EROS, Contrarian, future devices). Three facts bear on
where it should live:

1. **Licensing.** The motivation is escaping GPL-3.0 obligations inherited from
   NextUI's minarch. PlayOS today vendors nine patched GPL files under
   `minarch/overrides/`. A repository whose history contains no GPL-derived code
   makes provenance auditable at a glance; one that grows inside PlayOS does not.
2. **Consumption.** If several firmwares depend on it, it is a dependency by
   definition. Growing it inside one consumer and extracting it later is the
   most expensive ordering.
3. **Development loop.** The frontend needs a desktop backend to be developed
   at reasonable speed. PlayOS's build targets the device via a cross-compile
   container. These are different build lifecycles.

## Options considered

### Option A - inside PlayOS
Simplest start, no cross-repo coordination. But it entangles a
permissively licensed component with a repo that vendors GPL code, ties the
frontend's lifecycle to one device, and guarantees a painful extraction later.

### Option B - separate repository
Clean provenance, independent lifecycle, desktop-first development, consumable
by every firmware. Costs cross-repo coordination and a decision about *how*
consumers pull it in (submodule, vendored copy, or prebuilt artifact) - which
is not yet decided and is tracked in register §1 and §15.

### Option C - monorepo containing all firmwares plus the frontend
Would solve coordination, but means restructuring four existing working
projects - including Leaf, which has users and ships OTA updates - to solve a
problem that doesn't exist yet.

## Decision

Option B. The frontend gets its own repository, created once the project has a
name.

## Consequences

**Easier:** Provenance is self-evident. Desktop and device builds can diverge
freely. Other firmwares adopt it without inheriting PlayOS's structure.

**Harder:** Changes spanning frontend and firmware now cross a repo boundary.
The consumption model becomes a decision that must be made rather than avoided.

**Foreclosed:** Nothing permanently - merging into a monorepo later is possible,
just tedious.

## Revisit if

The frontend still has exactly one consumer when it reaches feature parity with
minarch. At that point the separation is carrying cost without delivering the
reuse that justified it.
