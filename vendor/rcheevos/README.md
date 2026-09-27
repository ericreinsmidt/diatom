# rcheevos, vendored

RetroAchievements' own implementation of their achievement condition language,
carried in under [ADR-0025](../../docs/decisions/0025-achievements-belong-to-the-frontend.md).

- Upstream: https://github.com/RetroAchievements/rcheevos
- Version: `f1417fb`, 2026-08-12
- **MIT licensed** - see `LICENSE`. Same license as diatom, which is what makes
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
| `rc_client.c` | manages a whole session including its own HTTP. ADR-0025 keeps networking in the launcher; diatom gains no network access. |
| `src/rapi/` | request builders for the RetroAchievements web API. Same reason. |
| `src/rurl/` | URL construction. Same reason. |
| `rc_libretro.c` | maps a `retro_memory_map` into RA's address space, but pulls in `rhash`'s internals for playlist and disc hashing that diatom will never call. Upstream's own Swift package excludes this file too. The region tables it would have used are in `rc_consoles.h`, which IS here, so diatom does that mapping itself against vendored data rather than vendoring 244K of CD reader and AES to reach it. |
| `rhash/` except `md5` | disc images, zip, AES, file readers. Identification happens in the launcher (`TortOS/tools/ra-check.py`), not here. |

The result is 30 source files, 18 of them compiled. Grep confirms what the
table claims: nothing vendored here calls `socket`, `connect`, `send`, `recv`,
`getaddrinfo` or any curl function.

**None of the 18 can be dropped.** Tested by elimination on 2026-08-29, one
file at a time and then in groups: every one fails to link. `rc_runtime` is a
single entry point onto achievements, leaderboards, rich presence and progress
serialization at once, and the parser reaches `format`, `value` and
`rc_validate` on any condition string. Even `md5` is reachable. So the size
below is the floor for using rcheevos at all, not a set of choices left
untaken.

## Building it

The build lists these files by name in the Makefile rather than globbing them:
a file appearing under `vendor/` should be a decision recorded in the table
above, not something the build picks up because it happens to be on disk.

Compile with `-DRC_DISABLE_LUA`. Diatom has no Lua and rcheevos' rich-presence
code is the only thing that wants it; upstream's own package sets the same
define. They are compiled without `-Wall -Wextra`: that is a rule diatom holds
itself to, and applying it to a dependency only produces warnings that cannot
be fixed here, because of the rule below.

## What it costs

Measured 2026-08-29, desktop build, `__TEXT` only:

| | machine code | source lines |
|---|---|---|
| diatom's own `src/` and `port/` | 42,548 | 4,634 |
| rcheevos as vendored | 77,517 | 11,529 |

**rcheevos is roughly 1.8x the size of the frontend it is going into**, which
trips ADR-0025's own "revisit if" clause. Recorded rather than smoothed over;
see that ADR for what the measurement did and did not change.

## Updating

Replace the files from a new upstream tag, keep the omissions above, and
re-run `make check` plus the achievement conform test. Do not patch anything
in this directory - a local fix here becomes invisible at the next update. If
something needs changing, change how diatom calls it.
