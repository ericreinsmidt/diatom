# 0032. Quiet the game in its own stream, when the launcher says so

- **Status:** Accepted
- **Date:** 2026-09-18 (accepted 2026-09-18)
- **Supersedes:** -
- **Superseded by:** -

Extends [ADR-0020](0020-shared-state-plane.md), which is unchanged: this adds a
state to the plane it defines and bends none of its rules. Sits beside
[ADR-0031](0031-mute-is-a-state-the-launcher-owns.md) and changes nothing in it.

## Context

TortOS now has an audio player, Muse: a daemon that plays music while a game
runs. Eric decided on 2026-09-18 that while music plays, **the game should be
silent, not mixed under it** - and that pausing the music brings the game's
sound back, which is also how a player switches between the two mid-game.

What constrains how:

1. **The two meet in dmix.** Muse opens ALSA's `default`, which on the Brick is
   dmix, and so does this port - ADR-0031 heard a second process's tone play
   through `default` while Diatom held the dmix slave.
2. **Everything below dmix is global.** `digital volume` and `HpSpeaker Switch`
   act on the mixed signal. ADR-0031 proved the switch silences every producer
   at once, which is exactly why the launcher can mute everything and cannot
   mute one thing. ADR-0031 also said, of Muse, that a producer's own silence
   belongs in its own stream. This is that sentence applied to Diatom.
3. **Every sample this host sends passes through one function.** `push` in
   `src/audio.c` is the only caller of `diatom_port_audio_write`, after the
   resampler and after rate control. Checked by reading, 2026-09-18.
4. **The launcher can write mid-game.** It turns `plat_resident_wait` at 10 Hz
   and already sends `SETIDLE` and `SETAUDIO` from there (ADR-0031, fact 5).
5. **A reopen is not free.** A named PCM drains and closes in about 105 ms
   (ADR-0030), against the 16.7 ms that ADR-0029 set as the bar for switching
   mid-game.

Not measured: whether cutting a game's stream to zero mid-sample clicks.
ADR-0031 found the hardware switch inaudible, but that is an analog stage, not
a step in the samples. Assumed that a step can click, which is why the
decision below fades.

## Options considered

### Option A - the launcher lowers a control below the mixer

`digital volume` to its floor, or the speaker switch off. Nothing new in
Diatom.

Rejected by fact 2: it silences the music with the game. It is not a weaker
version of what is wanted, it is the opposite of it.

### Option B - `SETLEVEL volume 0` for the game

An existing message.

Rejected for the same reason one level down. This port's volume IS the device's
level - the same control Muse plays through - and restoring it would put back
the launcher's idea of the volume over whatever the player chose in the game,
which is ADR-0031's Option B argument again.

### Option C - `SETAUDIO` to ALSA's `null` device

No protocol change at all: the game's sound goes to a device that discards it,
and back to the speaker when the music stops.

Plausible, and rejected. Every start and stop of the music becomes a reopen,
about 105 ms each (fact 5), so switching mid-game is a hitch where it should be
nothing. And it puts two questions into one state: `AUDIO` would report `null`
as where the sound is, and the launcher's output policy - jack, headset,
speaker - would have to remember what to restore underneath a second policy
that has nothing to do with it. Unverified as well: whether SDL paces against
the null plugin at all, or spins.

### Option D - pause the port's device while quiet

`SDL_PauseAudioDevice`, and unpause to bring the game back.

Rejected. The host keeps writing, and the port never blocks, so a paused device
fills its queue and drops everything after that - with rate control pinned at
its limit the whole time, because the buffer it steers by says full. Unpaused,
the queue plays up to 4096 frames of stale game audio, about 85 ms of whatever
was happening when the music started. It would also need a new port function
for a policy the port has no reason to know about.

### Option E - a state on the plane; this host writes the silence

`QUIET` and `SETQUIET`. While it is on, the samples `push` would have written
are replaced by silence. The stream keeps running at the same rate, rate
control steers exactly as before, the device is never reopened, and the port
does not change.

## Decision

**Option E. Quiet is a state on ADR-0020's plane, owned by the launcher, and
written by this host in its own stream.**

```
launcher → SETQUIET on=1      the game's sound is not wanted
launcher → QUIET              query
Diatom   → QUIET on=1         the reply; never sent unsolicited
```

ADR-0020's ownership table gains a row:

| State | Defined by | Written by | Diatom emits unsolicited |
|---|---|---|---|
| **Quiet** | **launcher** | **launcher** | **no** |

- **In the host, not the port.** `push` in `src/audio.c` substitutes silence
  after the resampler. The port sees a stream of samples either way, so
  ADR-0007's seam is untouched and `make check-seam` has nothing to check.
- **Faded, both ways.** A short ramp over a few milliseconds rather than a step,
  because of the assumption in Context. How short is measured on the device
  when it is built, not chosen here.
- **Held across `RUN` and across the pause.** Unlike the button map it is not
  reset by `RUN`: it describes the launcher's situation - something else is
  playing - and not the game. A launcher that keeps it current means a game
  started during the music is quiet from its first sample, with no race
  against the load. TortOS also sends it on every `RUN`, the way it sends
  `SETMUTE`, so a resident that restarted cannot start loud.
- **The launcher's policy, recorded here but not owned here:** quiet exactly
  while Muse is playing, followed at the 10 Hz turn. Pausing the music, or the
  album ending, brings the game back.

`QUIET` and `MUTE` are different things and must stay different. `MUTE` is the
launcher's cut of the analog stage, which silences every producer, and all this
host does about it is never undo it. `QUIET` is this host's own stream, and
nothing else is touched.

## Consequences

**Easier.** Music over a game without the game underneath it, switched with no
reopen and no hitch. The game never learns anything happened, and neither does
the port.

**Harder.** Another state and another parser on each side. And because it is
held across `RUN`, the failure is a silent game: a launcher that sets it and
never clears it leaves every game quiet with nothing playing. The launcher
therefore clears it whenever it cannot see Muse playing - including when Muse's
connection drops - and sends it on every `RUN` rather than trusting what is held.

**Forecloses:** a level. The game at 20% under the music - ducking - is not a
boolean, and this is one. If that is wanted it is a new ADR, not a widening of
this one.

## Revisit if

- Someone wants the game ducked rather than silenced.
- The device gains a per-client gain below the mixer, such as an ALSA softvol
  in front of dmix for each producer. Then the launcher could do this without
  Diatom, the way Option A wanted to.
- A producer other than Muse needs the game quiet on rules that disagree with
  Muse's, which would make one boolean the wrong shape.
