# tools

The checks `make check` runs, and the allocation counter `make conform` links
into a copy of diatom. None of it is part of diatom's runtime, builds as part of
`make`, or may be included from `src/` or `port/`.

The device instruments that produced the measurements cited in docs/ (the
probes, and the scripts that ran them on a Brick) are kept outside this
repository: they are for development on a device, not for building diatom.

## `check-rates.py`

Fails on any sample rate stated in the source or docs that the pinned cores, as
measured in docs/reference/core-facts.md, do not produce. Rates had been
recalled from memory and got wrong: a spike table once labeled 32040 as NES (it
is SNES).

## `fetch-cores.sh`

Verifies the cores CORES.md pins, fetching any that are missing from libretro's
own buildbot.

## `check-corefacts.py` - measured facts, not remembered ones

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

So docs/reference/core-facts.md is measured on the device, by a script kept
outside this repository, and `make check-corefacts` runs on every build.

The measuring script verifies every core's sha256 against CORES.md **on the
host, then again on the device after the push**, and refuses to measure
anything unpinned.
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

## Making a core die on purpose

`test/stubcore.c` reads `STUBCORE_CRASH` and kills itself on demand, which is
the only way to exercise `EXIT reason=crash` against `ERROR code=crash`. It
lives in the fixture rather than in diatom deliberately - a frontend with a
`--crash` flag would be test code inside the shipped binary, and the whole point
is that diatom learns about the crash the way it will in the field, from a
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

and drive the socket as test/proto.py does.

Two warnings earned the hard way, both on 2026-08-25. The recursion needs its
frame used *after* the call or the compiler turns it into a loop and the fixture
tests nothing - `volatile` and `noinline` are not enough. And give any watchdog
enough rope: a mode that does not crash must show up as a timeout, not as a
hung script.

## `--tap-audio` - the raw samples, both sides of the resampler

Not a tool in `tools/`, but it belongs with them. `diatom --tap-audio /tmp/x`
writes `x.in.raw` and `x.out.raw`: interleaved S16 stereo, exactly what the core
handed us and exactly what we handed the port. On the device `/tmp` is tmpfs, so
this is a RAM copy rather than an SD write in the frame path.

It exists because "the audio sounds wrong" needs to be attributable. Present in
IN means the core or the game; present only in OUT means us; present in neither
means downstream of us. That three-way split is what identified the resampler as
the source of the chirps on 2026-08-25, after a mic capture could not separate
the artifact from Contra's own gunfire.

The measurement that did it, on 15s of Contra:

| | HF energy >10 kHz | variability (cv) |
|---|---|---|
| IN (from the core) | 0.0055 | 1.63 |
| OUT (after resampling) | 0.0068 | **4.23** |

**18% of windows leave with MORE high-frequency energy than they arrived with.**
Linear interpolation is a lowpass and cannot add high frequencies; energy above
10 kHz that was not in the source is aliasing.

---

## allocwatch.c - what does diatom allocate, and when?

§11 is the section this project calls its own thesis, and its "no malloc in the
frame loop" item read **Unverified. Believed true.** for three days. This is what
turned it into a number.

Linked with `-Wl,--wrap=malloc` (and calloc, realloc, free), which rewrites those
calls in the objects being *linked*. A core arrives by `dlopen` and binds its own
malloc through libc, so it never appears here - and that is the right boundary,
because the claim under test is that *diatom* does not allocate per frame, not
that no code anywhere does. Cores allocate; several of them must.

The measurement is **differential**: run N frames and 2N frames and compare.
Start-up costs are identical in both, so anything the frame loop does is the
difference. That is what lets the whole thing live outside diatom - no counter to
arm, no hook to call, nothing test-shaped in the shipped binary, for the same
reason the crash fixture lives in `stubcore` rather than behind a `--crash` flag.

Measured on a Brick, 2026-08-26, FCEUmm running Contra:

| frames | diatom's allocations | peak RSS |
|---|---|---|
| 300 | `malloc=2 calloc=0 realloc=1 free=3` | 8.24 MB |
| 600 | `malloc=2 calloc=0 realloc=1 free=3` | 8.23 MB |
| 1200 | `malloc=2 calloc=0 realloc=1 free=3` | 8.44 MB |

**Three allocations, 126 kB, for the entire life of the process, all three
freed.** The claim was "nothing per frame"; the answer is very nearly nothing at
all. `make conform` links it into a copy of diatom, and `test/conform.py`
(`make conform-check`) runs that copy on any Linux machine.
