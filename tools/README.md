# tools

The checks `make check` runs, and the allocation counter `make conform` links
into a copy of diatom. None of it is part of diatom's runtime, builds as part of
`make`, or may be included from `src/` or `port/`.

The device instruments that produced the measurements cited in docs/ (the
probes, and the scripts that ran them on a Brick) are kept outside this
repository: they are for development on a device, not for building diatom.

## `fetch-cores.sh`

Verifies the cores CORES.md pins, fetching any that are missing from scutes
(github.com/ericreinsmidt/scutes), which keeps the exact files TortOS ships.

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
