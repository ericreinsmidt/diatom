# rcheevos, vendored

RetroAchievements' own implementation of their achievement condition language,
carried in under [ADR-0025](../../docs/decisions/0025-achievements-belong-to-the-frontend.md).

- Upstream: https://github.com/RetroAchievements/rcheevos
- Version: `f1417fb`, 2026-08-12
- **MIT licensed** - see `LICENSE`. Same licence as Diatom, which is what makes
  vendoring it compatible with ADR-0002 rather than in tension with it.

This is the second third-party thing in the repository, after `src/libretro.h`,
and it is the same kind of thing: someone else's definition of an interface
that has to be matched exactly. The condition language belongs to
RetroAchievements and changes on their schedule; an implementation written here
would be re-deriving a moving specification, and the failure mode of being
subtly behind is an achievement that never fires - which from the player's side
is indistinguishable from one they have not earned.

## What is here, and what is deliberately not

Vendored: `src/rcheevos/` (the evaluator), plus `rc_compat`, `rc_util`,
`rc_version` and `rhash/md5` which it needs, and the headers those use.

**Not vendored, on purpose:**

| omitted | why |
|---|---|
| `rc_client.c` | manages a whole session including its own HTTP. ADR-0025 keeps networking in the launcher; Diatom gains no network access. |
| `src/rapi/` | request builders for the RetroAchievements web API. Same reason. |
| `src/rurl/` | URL construction. Same reason. |
| `rc_libretro.c` | maps a `retro_memory_map` into RA's address space, but pulls in `rhash`'s internals for playlist and disc hashing that Diatom will never call. Upstream's own Swift package excludes this file too. The region tables it would have used are in `rc_consoles.h`, which IS here, so Diatom does that mapping itself against vendored data rather than vendoring 244K of CD reader and AES to reach it. |
| `rhash/` except `md5` | disc images, zip, AES, file readers. Identification happens in the launcher (`TortOS/tools/ra-check.py`), not here. |

The result is 32 files. A runtime-only test binary links at 56KB.

Grep confirms what the table claims: nothing vendored here calls `socket`,
`connect`, `send`, `recv`, `getaddrinfo` or any curl function.

## Building it

Compile with `-DRC_DISABLE_LUA`. Diatom has no Lua and rcheevos' rich-presence
code is the only thing that wants it; upstream's own package sets the same
define.

## Updating

Replace the files from a new upstream tag, keep the omissions above, and
re-run `make check` plus the achievement conform test. Do not patch anything
in this directory - a local fix here becomes invisible at the next update. If
something needs changing, change how Diatom calls it.
