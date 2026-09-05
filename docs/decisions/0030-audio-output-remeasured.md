# 0030. Audio output, re-measured: SDL cannot drive a bluealsa device string

- **Status:** Accepted
- **Date:** 2026-09-05
- **Supersedes:** [0029](0029-audio-output-is-a-state.md)
- **Superseded by:** -

Replaces [ADR-0029](0029-audio-output-is-a-state.md), whose decision survives
and is implemented, but three of whose facts did not. [ADR-0020](0020-shared-state-plane.md)
is unchanged; audio output is still the fifth state on its plane.

## Context

ADR-0029 was written from four measurements taken on 2026-09-03. Implementing
it on 2026-09-05 disproved three of the things it asserted, and the third was
load-bearing enough to have made the feature look broken for an evening.

### 1. SDL cannot drive `bluealsa:DEV=…,PROFILE=a2dp`

ADR-0029's finding (1) said `AUDIODEV` reaches SDL, "so in-process ALSA can
drive bluealsa". The first half is true. The conclusion is false.

Measured with `tools/btaudio.c`, which opens one device through SDL, pushes two
seconds of silence a period at a time, reports whether the queue drains, and
times the close. Three runs each way:

| device | queue after 2s | close |
|---|---|---|
| the default device (codec) | drains to 4096 bytes | returns in 198 ms |
| `bluealsa:DEV=…,PROFILE=a2dp` | climbs to 385024 bytes | never returns |
| a **named PCM** of `type bluealsa` | drains to 0 | returns in ~105 ms |

SDL opens the device-string form, reports a sane spec, and then its audio
thread never dequeues a byte. `aplay` drives the same sink correctly at every
period size from 1024 to 6000, so the sink, bluealsa, the pairing and the
headset are all healthy - `bluealsa` holds a steady 5460 kB RSS through thirty
seconds of it. Wrapping the string form in `plug` does not help.

The difference is only how the PCM is NAMED. The string goes through the
plugin's own argument parser; a named PCM goes through ALSA's normal config
path, and only the latter yields something SDL can write to.

**The other firmware's comment that ADR-0029 called untrue here was right**, and
that ADR's option C was rejected on a premise that does not hold. It stays
rejected, but for a different reason: a named PCM works, so nothing has to leave
the port.

Two consequences that are not obvious and cost hours:

- **alsa-lib caches its config at the first PCM open**, and tracks the files it
  actually read. A `.asoundrc` written when a headset connects is invisible to
  a resident emulator that opened the speaker at boot. The config must therefore
  be generated from the BOND, which is persistent and known at boot, rather than
  from the connection.
- It must be written to `HOME`. An adb shell has `HOME=/` and the resident
  emulator does not, which is how the first attempt passed by hand and failed
  in place.

### 2. Diatom does not hold the hardware PCM exclusively

ADR-0029's finding (4) said so. It holds the **dmix-backed default**. The
Brick's stock `/etc/asound.conf` routes `pcm.!default` through `softvol` into
`dmix`, and Diatom's substream reports `period_size 2048, buffer_size 8192` -
exactly the `PlaybackDmix` slave block - while the port asks SDL for 1024. A
direct `hw:` open would carry SDL's number through.

Verified directly: with Diatom holding the PCM, a second process played through
`aplay -D default` and exited 0.

The decision did not depend on this - finding (4) argued the two paths do not
fight, and they fight even less than it claimed - but it matters now, because
it is what makes a second audio producer possible at all.

### 3. Volume on a sink is reachable

ADR-0029's "Not in scope" said `--a2dp-volume` "leaves volume with the headset
deliberately, so a sink sits outside both ladders". The first clause is true and
the conclusion does not follow. Measured:

    amixer -D bluealsa scontrols
      'OpenRun Pro by Shokz - A2DP'   Limits: Playback 0 - 127   (at 127)

That is AVRCP absolute volume, exposed by `libasound_module_ctl_bluealsa.so`.

## Decision

**ADR-0029's decision stands unchanged.** Audio output is the fifth state on
ADR-0020's plane; the launcher owns which device, the port owns opening it, and
a failed or lost sink falls back to the default and says so rather than ending
the game. All of that is implemented and verified on hardware.

Three things are added or corrected:

**A sink is named, not described.** The device string a launcher sends must be
an ALSA PCM name that resolves through ALSA's config, not a plugin device
string. What generates that config is the host's business, exactly as which
sink to use is - the port still receives a name and still never learns what
kind of thing it names.

**Volume for a sink comes back into scope**, as a third window rather than an
extension of the ladder. The port already picks a raw window from the output -
speaker 0..39 and wired headphones 8..61, both inverted - and a sink is a third
window, 0..127 and NOT inverted, on a different mixer. ADR-0029's instruction
not to extend the 21-position ladder over a sink still stands; a third window is
not that. Not implemented.

**The fallback is the port's, and it is fast.** Traced with a headset switched
off mid-game: the port fell back within one 2-second sample and the codec's
`hw_ptr` advanced at real-time 48 kHz thereafter. The launcher's own poll
noticed 26 seconds later and produced one route change. So the launcher's
detection latency governs when its VIEW catches up, not when sound returns.

## Consequences

**Easier.** The failure that made this look impossible is understood and
one-line to avoid. `tools/btaudio.c` reproduces both halves in isolation, so the
next audio question costs minutes rather than an evening.

**Harder.** The host now has to generate ALSA config, which is a kind of state
it did not previously own, and get it in place before the emulator's first PCM
open. That ordering is invisible and will be rediscovered by anyone who does not
read this.

**Unchanged.** Everything in ADR-0029's decision section, and ADR-0020's plane.

## Revisit if

- SDL is replaced or updated on the device. The whole of finding 1 is a property
  of the SDL build in `/usr/trimui/lib`, not of ALSA or of bluealsa.
- A second audio producer appears. dmix makes it possible; nothing here has
  tested two producers plus a sink.
- The port gains its own ALSA path, which would make the named-PCM requirement
  moot and is the obvious answer if SDL disappoints again.
