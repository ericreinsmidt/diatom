# 0010. `RTLD_LOCAL` is mandatory, and load-bearing

- **Status:** Accepted
- **Date:** 2026-08-23
- **Supersedes:** -
- **Superseded by:** -

## Context

[ADR-0006](0006-keep-all-cores-resident.md) has every core `dlopen`'d and held
for the process lifetime. Its stated protection against symbol collision was
twofold: `RTLD_LOCAL` keeps names out of the global namespace, **and** an `nm -D`
check proving cores export nothing but `retro_*` - described there as *"a
stronger guarantee"*.

Its **revisit trigger** was: *any added core exports symbols other than
`retro_*`.*

**That trigger has fired.** Measured 2026-08-23 across six aarch64 cores:

| Core | Exports | `retro_*` | Other |
|---|---|---|---|
| fceumm | 45 | 45 | 0 |
| gambatte | 46 | 46 | 0 |
| mednafen_pce_fast | 53 | 53 | 0 |
| mgba | 25 | 25 | 0 |
| snes9x | 27 | 27 | 0 |
| **picodrive** | **1115** | 46 | **1069** |

PicoDrive exports 957 functions, 85 bss objects, 22 data objects and 5 rodata
objects beyond the API. Among them a **complete statically-linked zlib** -
`adler32`, `compress`, `crc32`, `deflate`, `gzopen`, `inflate`, `uncompress`,
`zlibVersion` - and internal names generic enough to collide with anything:
`Pico`, `cdd`, `ssp`, `svp`, `decode`, `tcache`, `MyFree`, `g_argv`, `crc16`.

Two things this does **not** mean:

- **Not a fault of PicoDrive.** This is a property of the *build*. The same
  source compiled with `-fvisibility=hidden` or a version script would be clean.
- **Not a reason to exclude it.** No other core exports those names, so there is
  no core-to-core collision here.

What it does mean: **the assumption that cores export only their API is false,
and cannot be assumed of any core.** Five of six being clean is not a pattern to
rely on - it is a sample.

## Options considered

### Option A - rely on the `nm` check, load cores `RTLD_GLOBAL`
Simpler in one respect: a core's dependencies resolve process-wide. But it
requires every core to be clean, which is now measurably untrue, and the failure
mode is silent - `crc32` binding to PicoDrive's statically-linked zlib rather
than the system one, process-wide, with no error anywhere.

### Option B - `RTLD_LOCAL` always, and treat the `nm` check as advisory
Symbols stay private to each handle; lookups go through `dlsym` on that handle.
Dirty cores become harmless rather than disqualifying. The `nm` check keeps
value as a *signal* about a build, not as a guarantee.

### Option C - reject cores that fail the `nm` check
Would exclude PicoDrive over a build flag, for a collision that `RTLD_LOCAL`
already prevents. Cost without benefit, and it makes Diatom picky about
something that is not its business.

## Decision

**Option B.** Every core is opened `RTLD_NOW | RTLD_LOCAL`. Never `RTLD_GLOBAL`,
anywhere, for any reason.

This is **load-bearing**: it is the single mechanism preventing symbol collision
between resident cores, not a redundant safeguard alongside a stronger one.
ADR-0006's characterisation of the `nm` check as "a stronger guarantee" was
correct for the three cores checked at the time and is not true in general.

The `nm` check is retained as **advisory** - worth running on a new core because
a large non-`retro_*` export set tells you something about how it was built, and
because it would catch a core exporting a name Diatom itself defines.

## Consequences

**Easier:** Cores no longer need to be well-behaved. A core that statically links
half of userspace is as safe as a clean one, so core selection stays the host's
decision on merit rather than on build hygiene.

**Harder:** Every symbol must be resolved with `dlsym` on a specific handle.
There is no process-wide fallback, so a core whose *undefined* references expect
a global definition would fail to load - which is the correct behaviour, loudly
rather than silently.

**The failure mode this prevents is silent.** With `RTLD_GLOBAL`, a second core's
`retro_run` can resolve to the first core's, and `crc32` can resolve to a
statically-linked copy with different behaviour. Neither produces an error. Both
present as inexplicable emulation bugs. That is why this is written down rather
than left to a code review.

## Revisit if

- A core cannot be loaded `RTLD_LOCAL` because it genuinely depends on
  global-scope resolution - evaluate that core, do not relax the rule; or
- Diatom itself needs to expose symbols *to* a core, which would require a
  deliberate, documented exception rather than `RTLD_GLOBAL`.
