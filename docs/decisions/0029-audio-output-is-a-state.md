# 0029. Audio output is a state, owned by the launcher

- **Status:** Superseded by [0030](0030-audio-output-remeasured.md)
- **Date:** 2026-09-03
- **Supersedes:** -
- **Superseded by:** [0030](0030-audio-output-remeasured.md)

> Its decision survives and is implemented; three of its facts did not. Finding
> (1) concluded that in-process ALSA can drive bluealsa, and SDL demonstrably
> cannot drive a bluealsa device string - only a named PCM. Finding (4) says
> Diatom holds the hardware PCM exclusively, and it holds the dmix-backed
> default. The "Not in scope" note rules out volume for a sink on reasoning that
> does not hold, because the headset's own control is reachable. Left unedited,
> because what was believed and why is the record.

Extends [ADR-0020](0020-shared-state-plane.md), which is unchanged: this adds a
fifth state to the plane it defines and bends none of its rules.

## Context

TortOS can now pair and connect a Bluetooth headset, and bluealsa offers its
A2DP sink as an ALSA device (`S16_LE, 2 channels, 48000 Hz` - already what a
game produces). Nothing routes a game's audio to it. Four things measured on
hardware on 2026-09-03 constrain how that can be fixed:

1. **`AUDIODEV` reaches SDL.** Setting it made SDL load the bluealsa ALSA plugin
   and genuinely attempt the sink; the failure came from inside
   `bluealsa-pcm.c`. So in-process ALSA can drive bluealsa. TortOS's other
   firmware carries a comment claiming it cannot; that is not true here.
2. **The port treats a failed audio open as fatal.** With `AUDIODEV` pointing at
   a sink that was not yet available, Diatom exited rather than falling back.
   The launcher stayed up with no resident emulator, so every launch would then
   pay ~1100 ms cold instead of ~15 ms warm. This is the worst available
   outcome and it happened by accident.
3. **The sink is not always available, and its availability is not Diatom's to
   know.** It depends on pairing, on a `bluetooth=1` setting, on a connection
   that comes and goes as a headset is powered on or walks out of range.
4. **The two paths do not contend.** Diatom holds the hardware PCM exclusively
   and bluealsa is a separate device; `aplay` fed the headset cleanly while a
   game played out of the speaker.

There is also a floor of roughly 100-150 ms of latency on Bluetooth audio - SBC,
the radio, and the headset's own buffer. Measured elsewhere, not by us, and
**accepted as a premise** on 2026-09-03: it is inherent to Bluetooth and not
something a frontend can or should try to correct.

## Options considered

### Option A - a fixed device at exec, via `AUDIODEV`

The launcher sets the environment variable when it starts the resident port.
Cheapest possible change; no protocol work at all.

Rejected, and it was tried. It cannot express "the speaker unless a headset is
connected", because the value is fixed before either is known. Combined with (2)
a wrong guess does not degrade, it removes the resident emulator. It also cannot
follow a headset that connects after launch, which is the ordinary case.

### Option B - the port detects the sink itself

`port/brick.c` watches for bluealsa and switches when it appears.

Rejected on the seam. The port would have to know about D-Bus, bluealsa, and
connection state - which is a device list by another name. The register's §12
says Diatom keeps no core list because which core runs a system is the host
application's config decision. Which sink carries the audio is the same shape of
decision, and answering it here would put a second such list in the one place
this project has repeatedly decided must not have one.

### Option C - the port writes PCM to a FIFO, something else routes it

Diatom stops opening an output device and writes raw frames to a pipe; a
separate process plays them wherever it likes.

Rejected, though it is the most honest-looking abstraction of the three. It
exists to work around the claim that (1) disproves, so its costs buy nothing
here: about 60 ms of additional buffering on top of the unavoidable floor, and a
process per game. It also moves a responsibility the port interface already owns
out of the port, which is a larger change to ADR-0007's seam than anything else
on this list.

### Option D - audio output becomes a state on the plane

`SETAUDIO <device>` writes it, `AUDIO <device>` reads it back and is emitted
unsolicited when the port changes it.

## Decision

**Option D. Audio output is a fifth state on ADR-0020's plane. The launcher owns
which device; the port owns opening it.**

ADR-0020's ownership table gains a row:

| State | Defined by | Written by | Diatom emits unsolicited |
|---|---|---|---|
| **Audio output** | **launcher** | **launcher** | **yes** |

The launcher owns it because it is the only side that knows a headset exists, is
paired, and is connected. The port owns opening it because opening a device is
what a port is for. The port is handed an ALSA device string exactly as it is
handed a core path, and never learns the word "bluetooth".

**A failed or lost sink falls back to the default device. It never ends the
game.** The port:

- opens the named device; if that fails, opens the default and keeps running
- emits `AUDIO <device>` for whatever it actually ended up on, unsolicited, so
  the launcher is never showing a sink that is not carrying sound
- does the same if a working sink dies mid-game, which is what happens when a
  headset is switched off or walks out of range

This half is not an optimisation. It is what makes a runtime-settable sink safe
to have at all: anything that can be set while a game runs can fail while a game
runs, and (2) is the current behaviour.

## Consequences

**Easier.** Connecting a headset mid-game can route sound to it without
relaunching. The launcher can show where audio is actually going, because the
port tells it rather than the launcher inferring. The fallback removes an entire
class of failure in which a missing device costs the resident emulator. Nothing
is added to the launch path: the state is written when something changes, not on
every `RUN`.

**Harder.** The port now has an audio device that can change under it, so
reopening has to be safe against the audio thread - a concurrency question the
port did not previously have. `AUDIO` is a fifth parser on the launcher side.
And the fallback means the port can be somewhere the launcher did not ask for,
which every consumer of that state must handle rather than assume.

**Foreclosed.** The port choosing its own output. If a future device wants
automatic routing, that logic belongs in the host application, and this decision
says so.

**Not in scope.** Volume for a Bluetooth sink. `--a2dp-volume` leaves volume with
the headset deliberately, so a sink sits outside both ladders in the port and the
21-position ladder must not be extended over it.

## Revisit if

- The port's audio reopen is measured to cost more than one frame at 60 Hz
  (16.7 ms) on the device. Then switching mid-game is not free and should be
  restricted to moments where a hitch is already expected.
- A second consumer of Diatom needs automatic sink selection with no host to ask.
  That would be the first real argument for Option B.
- `AUDIODEV`-style fixed selection becomes sufficient because the port learns to
  survive a failed open AND sinks stop appearing after launch. Both would have to
  become true; either alone leaves Option A broken.
