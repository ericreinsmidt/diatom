# 0031. Mute is the launcher's, and no producer may undo it

- **Status:** Accepted
- **Date:** 2026-09-16 (accepted 2026-09-17)
- **Supersedes:** -
- **Superseded by:** -

Extends [ADR-0020](0020-shared-state-plane.md), which is unchanged: this adds a
sixth state to the plane it defines and bends none of its rules.

## Context

The Brick has a physical switch that nothing reads. TortOS was asked to make it
mute. Measured on hardware 2026-09-16, by watching every input device and every
exported GPIO while it was flipped, and then by listening:

1. **The switch is `gpio243`, and it also reports as `EV_SW` code 1 on
   `/dev/input/event3`.** The GPIO is what TortOS reads, because an event only
   arrives on a CHANGE and says nothing about which way the switch points at
   boot, after a resume, or after the launcher restarts - which is exactly when
   a physical control and the software can disagree.
2. **Down reads 1, and down is muted.** Eric's call: "off" is what the position
   says, and on a mute switch that labels the sound.
3. **`HpSpeaker Switch` sits below the mixer and cuts everything.** Proved by
   playing a tone from a SECOND process through `default` while Diatom held the
   dmix slave, and flipping the switch: the tone stopped and came back. Not
   inferred from the topology - heard.
4. **No click.** The cut and the restore are both inaudible mid-stream, which is
   why this state is a boolean and not a level with a ramp.
5. **The launcher is not blocked during a game.** `plat_resident_wait` turns at
   10 Hz - `dline(100)` - so TortOS can read the switch while a game runs. An
   earlier draft of this ADR assumed it could not and built a much worse design
   on top of that.
6. **The fault is real, not predicted.** With TortOS's half shipped and a game
   running, the switch muted it within 100 ms. Pressing a volume key then
   brought the sound straight back, switch still down - because Diatom wrote
   `HpSpeaker Switch` as part of applying the new level. Predicted from the
   code first and then produced on demand, which is the only reason this ADR
   describes a mechanism rather than a worry.

Two facts about the device that shaped the rest:

**The launcher can produce audio.** TortOS opens no audio device today, but
nothing stops it: the Brick's `/etc/asound.conf` routes `default` through dmix
and Diatom holds the dmix-backed slave, not the raw PCM. (That also corrects
ADR-0029, which says Diatom holds the hardware PCM exclusively. It does not.
Recorded in TortOS's own scoping for the audio player, and re-confirmed here by
the tone.)

**There will be three producers, not two.** Muse - the audio player, named
after Eric's own 0BSD player in EROS - is scoped as a daemon over a socket, and
one of its goals is **playing music over a running game**. So game audio and
music will mix at dmix, above one analog output stage.

And the fault that makes this an ADR rather than a patch: **a producer
legitimately cuts that same control for its own silence.** Diatom does it at
level 0, and its comment says why - the control's minimum is about -74 dB,
which an ear against the speaker can still hear, so level 0 has to cut the path
rather than attenuate. It also turns the speaker unconditionally back ON when
it exits, reasoning that "the launcher drives `digital volume`, NOT this
switch, so turning the volume up there cannot undo it". That was true when it
was written. TortOS now drives the switch, so it is not true any more.

## Options considered

### Option A - each producer reads the switch itself

The headphone jack precedent: both sides poll the hardware, neither tells the
other anything. No protocol change.

Rejected. A jack is a **routing fact** - it changes where sound goes and which
gain window is correct, so a producer cannot do its job without it. A mute
switch is a **policy**: which position means quiet, whether it is a mute or a
hold, what happens when it disagrees with software after a resume. That is the
launcher's kind of decision, of a kind with Auto Off.

It also scales the wrong way. With Muse there are three processes that would
each need the pin number, the polarity and the same answer, with nothing
keeping them in step.

### Option B - the launcher sends `SETLEVEL volume 0`

No new verb.

It cannot unmute. Diatom owns the volume while a game runs and the player can
change it in-game, so only Diatom knows what to restore. Restoring the level
TortOS last sent would silently undo what the player chose.

### Option C - the launcher owns the cut; producers must not undo it

TortOS reads the switch everywhere, including inside `plat_resident_wait`, and
cuts `HpSpeaker Switch` itself. Because that control is below the mixer, one
write silences every producer at once - game, music, and anything later.

A producer may still cut the path for its OWN level 0, because that is its own
correctness and has to hold when nothing else is running. What it may not do is
turn the speaker back ON while the launcher says muted. That is the single rule,
and `SETMUTE` is how a producer learns the state.

## Decision

Option C.

- **TortOS owns the switch and the cut.** It reads `gpio243`, decides that down
  means muted, and drives `HpSpeaker Switch`.
- **`MUTE` reports and `SETMUTE on|off` writes**, the query doubling as the
  change event, per ADR-0020. TortOS sends it on change and on `RUN`, so a game
  started with the switch already down begins quiet.
- **Diatom keeps cutting the path at its own level 0** and stops re-enabling the
  speaker while muted - including in its exit path, whose comment must be
  corrected rather than left asserting something that is no longer true.
- **Muse is bound by the same rule before it exists.** It needs nothing to BE
  muted, since the cut is below it; it must simply never write
  `HpSpeaker Switch` to turn the speaker on. Its own silence belongs in its own
  stream.

## Consequences

Easier: one place decides what the switch means. A second device without a
switch changes nothing in any producer. Muting is global by construction, which
is what a hardware switch should do - flip it with a game and music both
playing and everything stops.

Harder: a rule that spans three processes is only as good as the newest one
honoring it. This ADR is the only place it is written down, and Muse does not
exist yet to be checked against it.

Also harder: `SETMUTE` has to survive the hazards every plane state does - one
arriving while a core is loading, or between `RUN` and the first frame, has to
hold until there is audio to apply it to.

Forecloses: muting one producer and not another. That is per-source volume, a
different feature, and this switch is not it.

## Revisit if

A device ships whose mute is not readable by the launcher, or a producer appears
that must keep making sound while the switch says quiet - an alarm, or a call.
Either turns the single global cut into per-source policy and reopens this.
