# tools

Measurement instruments, not part of Diatom's runtime and never linked into it.

They started as spikes. They are tracked because their own results are cited as
evidence in `docs/spikes/`, and an unversioned instrument makes those claims
unverifiable - which is the exact failure this project exists to avoid. A spike
is a throwaway that answers one question and dies; an instrument is something you
re-run to re-check a claim. These became the second the moment the register
created work items requiring them.

Neither builds as part of `make`. Neither may be included from `src/` or `port/`.

## `envlog.c`

*Which `RETRO_ENVIRONMENT_*` calls does a core actually make, and at which
lifecycle phase?*

Loads a core, logs every environment command with its phase, answers a minimal
set and declines the rest - a decline is a data point. Then `retro_init`,
`retro_load_game`, 120 frames, `retro_unload_game`, `retro_deinit`.

Produced [the environment inventory](../docs/spikes/2026-08-23-env-inventory.md):
34 of 77 commands appear, ~17 need real answers, 17 are declined by every core
with nothing breaking.

**Re-run when a core is added**, to check it asks for nothing new.

## `rssprobe.c`

*What does holding every core resident cost in RSS, and what does `dlopen` cost
on this CPU?*

Maps N cores `RTLD_NOW | RTLD_LOCAL`, timing each, initialises exactly one, loads
a game, runs 720 frames, and reports resident memory at every step.

Produced [the RSS and dlopen measurements](../docs/spikes/2026-08-23-rss-and-dlopen.md)
and evaluates [ADR-0006](../docs/decisions/0006-keep-all-cores-resident.md)'s
revisit trigger. **Must run on the target device** - timings from a faster CPU
are worse than no measurement, because they look authoritative.

## `gen_env_names.py`

Generates `env_names.h` from `src/libretro.h`. Generated rather than committed so
the table cannot drift from the header it describes.

## Running them

Cores and ROMs are supplied locally and are deliberately not in this repository:
cores are third-party binaries, ROMs are copyrighted.

```sh
make tools                    # builds for the host
make tools PORT=brick         # cross-builds in the Brick toolchain container

./build/tools/envlog   <core.so> [rom]
./build/tools/rssprobe <rom> <core.so>...
```

Set `LD_LIBRARY_PATH` if the cores need runtime libraries beside them.
