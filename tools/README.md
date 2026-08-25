# tools

Measurement instruments, not part of Diatom's runtime and never linked into it.

They started as spikes. They are tracked because their own results are cited as
evidence in `docs/spikes/`, and an unversioned instrument makes those claims
unverifiable - which is the exact failure this project exists to avoid. A spike
is a throwaway that answers one question and dies; an instrument is something you
re-run to re-check a claim. These became the second the moment the register
created work items requiring them.

Neither builds as part of `make`. Neither may be included from `src/` or `port/`.

One file here is not an instrument: `check-register.py` is a **check**, and it
runs as part of `make check` alongside `check-seam`. See the bottom of this
file.

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

## Device probes

Small single-question instruments that only run on the target. Built with
`tools/brick-make.sh probes`, not by `make`. **Every number they produce is
cited by an ADR** - which is the whole reason they are versioned rather than
left in a temp directory. A measurement whose instrument has been deleted
cannot be re-checked, and that is the drift this project exists to avoid.

| Probe | Question | Cited by |
|---|---|---|
| `savprobe.c` | What does each core expose for saving - SRAM, RTC, state size - and what does `retro_serialize` cost? | [ADR-0016](../docs/decisions/0016-saves-and-save-states.md) |
| `wprobe.c` | What does an atomic save write (write, fsync, rename) cost on the device's card? | [ADR-0016](../docs/decisions/0016-saves-and-save-states.md) |
| `sigprobe.c` | On power-off, does a running process get a signal, and how long before it dies? | [ADR-0016](../docs/decisions/0016-saves-and-save-states.md) |
| `protodrive.c` | A stand-in launcher: drives Diatom over the ADR-0009 socket and times RUN to RUNNING. Also exercises READY, STOP and QUIT, so a protocol regression fails here rather than on a device with a real launcher attached. | [protocol log](../docs/discussion/2026-08-25-protocol.md) |
| `blitprobe.c` | Is the blit slow because of the pixels it computes or the memory it writes? | [blit-cost spike](../docs/spikes/2026-08-25-blit-cost.md) |
| `warmprobe.c` | What does a launch cost with the process up and the core already resident? | [warm-launch spike](../docs/spikes/2026-08-25-warm-launch.md) |
| `pantest.c` | What does `FBIOPAN_DISPLAY` cost, and does anything change it? | [ADR-0013](../docs/decisions/0013-brick-fbdev-flip-thread.md) |
| `eglpresent.c` | Stand-in for the launcher: presents through the device's mali/EGL driver. | [handoff spike](../docs/spikes/2026-08-24-display-handoff.md) |
| `holdfb.c` | Can EGL present while another process holds `/dev/fb0` open and mapped? | [handoff spike](../docs/spikes/2026-08-24-display-handoff.md) |

`savprobe` needs a core and a ROM; the rest need neither. `holdfb` and
`eglpresent` are two halves of one test and are meant to overlap in time - see
the spike for the sequence.

`protodrive` with `secs` of **0** does not send STOP: the session is left to end
on its own. That is how the crash paths are watched, since the point there is
that Diatom reports something nobody asked it to.

## Making a core die on purpose

`test/stubcore.c` reads `STUBCORE_CRASH` and kills itself on demand, which is
the only way to exercise `EXIT reason=crash` against `ERROR code=crash`. It
lives in the fixture rather than in Diatom deliberately - a frontend with a
`--crash` flag would be test code inside the shipped binary, and the whole point
is that Diatom learns about the crash the way it will in the field, from a
signal it did not raise.

| Value | What it does |
|---|---|
| `segv[@N]` | null dereference at frame N (default 60) |
| `stack[@N]` | unbounded recursion, which needs the handler's alternate stack |
| `abort[@N]` | `abort()`, so SIGABRT |
| `fpe[@N]` | integer divide by zero - **a no-op on ARM**, which does not trap it |
| `exit[@N]` | `exit(1)`, which raises no signal at all |
| `load` | dies inside `retro_load_game`, before RUNNING |

    STUBCORE_CRASH=segv@60 ./diatom --socket /tmp/dc.sock &
    ./protodrive /tmp/dc.sock 0 "/path/to/stubcore.so|"

Two warnings earned the hard way, both on 2026-08-25. The recursion needs its
frame used *after* the call or the compiler turns it into a loop and the fixture
tests nothing - `volatile` and `noinline` are not enough. And give any watchdog
enough rope: a mode that does not crash must show up as a timeout, not as a
hung script.

## Running them

Cores and ROMs are supplied locally and are deliberately not in this repository:
cores are third-party binaries, ROMs are copyrighted.

```sh
make tools                    # builds for the host
tools/brick-make.sh tools     # cross-builds in the Brick toolchain container

./build/desktop/tools/envlog   <core.so> [rom]
./build/desktop/tools/rssprobe <rom> <core.so>...
```

Cross-built instruments land in `build/brick/tools/` and run on the device.

## Brick toolchain

The cross build for the TrimUI Brick (TG3040), per
[ADR-0012](../docs/decisions/0012-independent-toolchain.md):

- `brick-toolchain.Dockerfile` - the container image: Debian bullseye pinned by
  digest, stock `aarch64-linux-gnu` GCC. Build once:
  `docker build -f tools/brick-toolchain.Dockerfile -t diatom-brick-toolchain tools`
- `fetch-brick-sysroot.sh` - regenerates `sysroot/brick/` from first sources:
  the device's own libraries over ADB, SDL2 headers from the upstream release,
  pinned by sha256. Needs a connected Brick.
- `brick-make.sh` - runs `make PORT=brick` inside the container.
- `brick-run.sh` - builds nothing; pushes the current build and runs it on the
  device with exclusive use of the display. Arguments pass through to diatom.
- `brick-device-run.sh` - the on-device half, staged as
  `/mnt/SDCARD/diatom/run.sh`. Freezes the PlayOS supervisor, kills the UI,
  runs, and restores in a trap.

Taking the display on this device has two traps in it, both hit for real
(ADR-0013 and the 2026-08-24 session log): adbd runs under the PlayOS launch
chain, so sweeping that process group severs ADB; and `launch.sh` is a
supervisor that respawns the UI, so killing the UI alone leaves two processes
presenting and wedges the GPU firmware in-kernel. Both need a power cycle to
recover. The scripts exist so that sequence is never retyped from memory.

```sh
tools/brick-run.sh                          # staged NES core and ROM
tools/brick-run.sh --display stretch        # a specific starting mode
tools/brick-run.sh --core X.so --rom game --frames 600 --shot /tmp/x.bmp
```

Set `LD_LIBRARY_PATH` if the cores need runtime libraries beside them.

## `check-register.py` - a check, not an instrument

*Can the register still be trusted to say what is left?*

By 2026-08-25 it could not. It had grown 418 to 992 lines in three days and
**never once shrunk**, because every resolution was appended rather than
substituted: section 10 opened with five unticked questions and then, twelve
lines down, a `RESOLVED -> ADR-0006` block ticking the same five. The
core-options work created `## 4c` and left `## 12` standing. Saves did the same
thing across `## 5b` and `## 9`.

That was not neglect - the register was edited in **15 of the 16 ADR commits**.
It was a habit. Writing the resolution is the satisfying part; deleting the
question it answers feels like discarding information, in a project whose whole
discipline is to keep the reasoning. But the reasoning lives in the ADR. The
register only has to say what is left.

So it exists for the same reason `check-seam` does. That rule has held since
day one because a build fails when it is broken, not because anyone remembered
it. Nothing ever failed when the register drifted, so it drifted.

    make check              check-seam plus this
    make check-register     this alone
    make check-register-diff   enforce substitution over appending

**FAIL** - structural facts a machine can be certain about:

| | |
|---|---|
| Section order | `## 5` sitting above `## 4b` is how you see sections were inserted wherever was convenient |
| Resolved but still open | a section recording a resolution that still carries unticked items above it - the exact signature of the habit |
| Duplicate subject | two sections whose titles cover the same thing |
| Undeclared tag | the header once said "nothing is decided unless it says DECIDED" while every resolution was written `RESOLVED`; the stated vocabulary and the real one drifted apart unnoticed |
| Orphaned ADR | an Accepted ADR that no ticked item points at |
| `--diff`: appended a resolution | a change that records a resolution and deletes nothing |

**WARN** - suspected fossils, an open item whose vocabulary is already covered
by a ticked one elsewhere. Never fails a build, because "is this the same
question?" is a judgement. The first version scored these on raw shared-word
count and produced **40 warnings, nearly all noise** - `**[OPEN]**` was leaking
into the comparison so every open item matched every other. Tags are stripped
first now, and overlap is measured against the *shorter* item rather than the
union, because a one-line question and a six-line resolution can be about
exactly the same thing. That took it to 4, all worth reading. A check that
cries wolf is one people learn to ignore, which is worse than no check.

## `corefacts.sh` and `check-corefacts.py` - measured facts, not remembered ones

*Do the numbers in `docs/` still describe the cores we actually ship?*

On 2026-08-25 an audit found that **three of the six rows in ADR-0015's aspect
table described cores the project had rejected the day before.** Genesis went
1.3333 to 1.5238 when PicoDrive lost to Genesis Plus GX; SNES 1.3333 to 1.5842
when snes9x 1.63 lost to snes9x2010; NES 1.2190 to 1.3061 on a rebuilt FCEUmm.

Nothing could detect it, because that table was keyed on the **system**. A
system does not report an aspect ratio - a specific core build does, and the
number had been separated from the binary that produced it. CORES.md already
stated the intent: *"this file exists so the measurements in docs/ name the
exact bytes that produced them."* Stating it was not enough.

    tools/corefacts.sh           re-measure and rewrite docs/reference/core-facts.md
    tools/corefacts.sh --check   re-measure and fail if the file is out of date
    make check-corefacts         the offline half; runs on every build

`corefacts.sh` verifies every core's sha256 against CORES.md **on the host, then
again on the device after the push**, and refuses to measure anything unpinned.
That is not belt-and-braces. The hand audit that started this made exactly that
mistake: it measured a device staging directory that had drifted, read mGBA's
rate as 48000 when the pinned build reports 65536, and wrongly called the Game
Boy row stale. A careful audit was still wrong, because carefulness cannot
verify a hash.

`check-corefacts.py` is what runs without a device. It asserts one thing - that
the pinned set recorded in `core-facts.md` is still the pinned set in CORES.md -
so swapping a core fails immediately instead of quietly aging every measurement
that cited it.

A first version also tried to forbid docs from restating any number the facts
file owns. It fired on `50.0070` a dozen times, which is not a core's opinion
but the correct PAL frame rate. Removed: a check that cannot tell a legitimate
reference from a stale copy is one people learn to ignore. Citing the file by
link is therefore a **convention**, and the tooling says so rather than
pretending to enforce it.

## `micprobe.sh` - did sound actually come out?

*Answered as a number, by the device's own microphone.*

The Brick's capture device sits on the same card as playback, and **its mic
hears its own speaker**. So "is there audio" stops being a question only a
person in the room can answer:

    silence           rms   ~25-36
    Probotector       rms  ~700-780
    full-scale sine   rms ~1565

    tools/micprobe.sh <core.so> <rom> [secs]
    tools/micprobe.sh --tone                  known-good reference
    DIATOM_GAIN=15 tools/micprobe.sh ...      volume; INVERTED, lower is louder

It records a baseline first, then records again while the thing under test
plays, and reports the ratio. Four consecutive Diatom runs measured 686-702
against a 25 baseline - which is what finally established that Diatom's audio
worked, after hours of assuming it did not.

**Why it exists.** On 2026-08-25 an evening went into "no sound" that was never
a fault. Every report from the person listening was accurate and consistent -
*"very quiet"*, *"barely audible"*, *"still quiet at max"* - and every one of
them described a **level**. Without a baseline to compare against, none could be
acted on, so they were treated as symptoms of a defect instead: the resampler
was suspected, then the cores, then SDL, and a rewrite of the audio path was
nearly proposed. The audio path was fine throughout. `digital volume` is
inverted, so what was set as "maximum" was silence.

*"Still quiet at max"* is a contradiction and should have ended it in one step,
because it can only mean max is not max. It did not, because there was no
instrument - only opinions about opinions.

The lesson is not to listen more carefully. It is that **"does this sound right"
has a number behind it**, and that number was ten minutes of work away using
`arecord`, which was on the device the whole time.

## `--tap-audio` - the raw samples, both sides of the resampler

Not a tool in `tools/`, but it belongs with them. `diatom --tap-audio /tmp/x`
writes `x.in.raw` and `x.out.raw`: interleaved S16 stereo, exactly what the core
handed us and exactly what we handed the port. On the device `/tmp` is tmpfs, so
this is a RAM copy rather than an SD write in the frame path.

It exists because "the audio sounds wrong" needs to be attributable. Present in
IN means the core or the game; present only in OUT means us; present in neither
means downstream of us. That three-way split is what identified the resampler as
the source of the chirps on 2026-08-25, after a mic capture could not separate
the artefact from Contra's own gunfire.

The measurement that did it, on 15s of Contra:

| | HF energy >10 kHz | variability (cv) |
|---|---|---|
| IN (from the core) | 0.0055 | 1.63 |
| OUT (after resampling) | 0.0068 | **4.23** |

**18% of windows leave with MORE high-frequency energy than they arrived with.**
Linear interpolation is a lowpass and cannot add high frequencies; energy above
10 kHz that was not in the source is aliasing.
