# The audio queue overshot its own capacity, and that was the small half

`audio_write` was refusing a batch only when the queue was **already full**, so
a full batch could land against a nearly-full queue. Measured on the Brick:
**4927 frames against a stated capacity of 4096.**

That mattered beyond the extra 17 ms of latency. Rate control computes its error
as `(queued - target) / target` and clamps it at **+1.0**, which is reached at
exactly capacity - so everything above capacity was invisible to the controller
whose job was to prevent it.

The fix is three lines: take only what fits. Clamping rather than refusing the
whole batch loses less, because the frames that do not fit are dropped either
way and there is no reason to discard the ones that would have.

## The change that made the real problem visible

`diatom_port_audio_write` returned `void`, so "drops on overflow" was
unobservable - the host could not tell a dropped frame from a written one, and
the only evidence was queue depth, which the overshoot was hiding. It now
returns the number of frames accepted, and the host counts and reports the
difference.

That is not a nicety. The clamp *trades* queue overshoot for dropped frames, so
without counting them there was no way to tell whether the fix made audio worse.

A/B on one device in one session, one binary differing only in the clamp:

| | max queue | dropped |
|---|---|---|
| NES PAL, unclamped | 4927 | 3825 |
| NES PAL, clamped | **4096** | 4120 |
| SNES, unclamped | 4671 | 1915 |
| SNES, clamped | **4096** | 2763 |
| Genesis, unclamped | 4587 | 801 |
| Genesis, clamped | **4096** | 1545 |

The clamp works exactly as intended. It also revealed that **thousands of frames
were already being dropped and nobody could see it** - 3825 on NES before any
change was made.

## Where they were going

Drift never exceeded 0.2% of a 0.5% budget, so rate control was not saturated
and had authority to spare. That ruled out a systematic imbalance and left a
transient. One measurement settled it:

    300 frames  dropped 4120
    600 frames  dropped 4120
    1200 frames dropped 4120
    2400 frames dropped 4120

**Identical at every duration.** Entirely a startup transient, nothing in steady
state. Five consecutive refusals, all at `q=4096`, all core-sized batches of
~960 - so not the 512-frame silence chunks from priming.

The arithmetic then names it exactly:

| | queued |
|---|---|
| prime to half capacity | 2048 |
| warmup frame 1 | 3008 |
| warmup frame 2 | 3968 |
| warmup frame 3 | 4096, **831 of 960 refused** |

The three warmup frames run **unpaced** - back-to-back `retro_run` calls that
exist to fault in code paths and prime the texture, timed at 23-34 ms in total.
Their audio was being queued. So the buffer reached 97% full before the paced
loop ran a single frame, and the next few frames had nowhere to go.

## The second fix

Warmup audio is now discarded. It is pre-roll for a game that has not started,
produced by calls with no correct timing, and dropping it deliberately is more
honest than queueing it and having the port refuse it.

| System | before | after |
|---|---|---|
| NES PAL | 4120 | **1244** |
| SNES | 2763 | **159** |
| GBA | 153 | **0** |
| Genesis | 1545 | **0** |
| PC Engine | - | 575 |

All still at 0 resyncs and on-target frame rates. The NES remainder is constant
at 300, 900 and 1800 frames, so it is still purely a transient - about 26 ms of
audio once per launch, down from 86 ms.

## What is left, stated rather than glossed

The remaining 1244 (NES) and 575 (PC Engine) are the same shape one step
smaller: priming fills to 2048 of a 4096 buffer, leaving about two frames of
headroom, and the first paced frames can lead the device's consumption by more
than that. Priming to a quarter would leave three frames of headroom and trade
against underrun risk at startup, which is what priming exists to prevent.

Not tuned here, because it is a tuning question with its own failure mode and
the transient is now bounded, measured and constant. It is worth doing when
someone can listen to the result rather than read a number.

## Method note

The first A/B produced two binaries with **identical hashes**. The container had
not rebuilt - the Docker mtime problem the staleness guard exists for - and I
hit it by calling `docker run` directly instead of going through
`tools/brick-make.sh`, which is where the guard lives. The check for it is
cheap: compare the two hashes and refuse to interpret the run if they match.
Second time this project has bypassed its own guard by reaching past the script
that holds it.

## Verified on hardware, 2026-08-25

**Nothing audible is attributable to Diatom.** Confirmed by listening on the
device, and the route to that answer is worth more than the answer.

A chirp was reported on NES and initially read as a defect in this work. It is
not. The `--tap-audio` comparison put it **in the core's output before Diatom
touches a sample**, and it is present in **minarch on the same hardware playing
the same game** - so it is inherent to Contra under FCEUmm, not to anything
here. It only became noticeable once the output gain was finally set correctly.

Three hypotheses died on the way, all of them mine, all of them from a metric
that did not survive contact:

- **Aliasing in the resampler.** Claimed from HF as a *fraction* of total
  energy, which rises whenever the low end drops. Absolute HF energy comes out
  at **0.756** of what goes in - a lowpass behaving correctly. No aliasing.
- **Ongoing drops.** Contra drops 682-687 frames at 5s, 15s, 30s and 60s:
  constant, so startup only.
- **Sample-to-sample jump counting.** IN had *more* jumps than OUT (266/s vs
  162/s), because NES square waves are discontinuous by construction and the
  detector could not tell an edge from an artefact.

One real bug was found and fixed on the way - `g_prev` was not carried through
the pass-through path - but measurement showed it was not the reported symptom,
and the commit says so.

**The method that worked**, after several that did not: capture the signal at
each boundary and play the captures back. `--tap-audio` writes both sides of the
resampler; converting them to WAV and playing them through `aplay` answered in
one listen what an evening of metrics could not. Attribution beats analysis when
the question is "whose artefact is this".

## What it turned up that is not a bug

FCEUmm ships `fceumm_sndquality = Low`, with "Reduce Triangle Channel Popping"
and "Reduce DMC Channel Popping" both disabled. Very High plus both cut large
sample-to-sample jumps by **42%** on Contra, 138/s to 80/s.

That is not a defect - minarch runs the same defaults - but it is a live
question about who owns core-option defaults, recorded in §6. Diatom has no core
list, so it cannot hold per-core opinions without breaking a stated principle.
