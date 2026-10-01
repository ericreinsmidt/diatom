# 0034. Fast forward on MENU+R1, drawn at half rate and muted

- **Status:** Proposed
- **Date:** 2026-09-30
- **Supersedes:** -
- **Superseded by:** -

Settles the question [ADR-0014](0014-display-modes-and-default.md) left open,
who owns in-game hotkeys, for the one hotkey there is a case for. Extends
[ADR-0020](0020-shared-state-plane.md) with one state Diatom owns, and keeps
[ADR-0019](0019-input-mapping-and-remapping.md)'s rule that hotkeys read the
physical pad, never the remapped one.

## Context

Eric asked for fast forward in games. Rewind was asked about in the same breath
and set aside: it may never come, and nothing here depends on it.

Measured on the Brick 2026-09-30:

1. **Core time, unpaced, from real gameplay states** (`tools/ffprobe`, 1,800
   frames, three passes within about 3%, CPU at its 2.0 GHz ceiling):

   | core | game | p50 frame | p95 frame | p95 as a speed |
   |---|---|---|---|---|
   | snes9x2010 | Donkey Kong Country | 4.24 ms | 4.67 ms | 3.6x |
   | genesis_plus_gx | Gunstar Heroes | 3.14 ms | 3.60 ms | 4.6x |
   | mgba | Golden Sun, a battle | 2.90 ms | 3.66 ms | 4.6x |
   | mednafen_ngp | Metal Slug 1st Mission | 3.37 ms | 3.44 ms | 4.8x |
   | fceumm | Contra | 2.17 ms | 2.34 ms | 7.1x |
   | mednafen_pce_fast | Rondo of Blood (CD) | 1.51 ms | 2.16 ms | 7.7x |

2. **Presenting is not free, and it is on the frame loop's thread.** The port
   scales into a page on the caller's thread and only the pan is the flip
   thread's (ADR-0013). The exit summary measured it at 6.65 and 6.96 ms a
   frame in `stretch`, two Sonic sessions. A refresh is 16.7 ms, so drawing
   every refresh leaves about 9 ms of it for the core - SNES under 2x.

3. **R2 is taken on three systems.** `ffprobe` with `FFPROBE_INPUTS=1` lists
   what each core declares and counts what it reads: mGBA has R2 as Turbo R on
   GB, GBC and GBA, and Eric confirmed it repeats R in a GBA game; fceumm has it
   as VS System Insert Coin. R1 is a real button on SNES, GBA and the Genesis
   six-button pad. SELECT is a real button almost everywhere. MENU is the only
   button no core sees: `button_map` sends it nowhere (`src/env.c`).

4. **Audio is paced to real time.** `diatom_audio_sync` holds the port's queue
   at half capacity with a proportional-integral controller. Feeding it two to
   four times the samples, or none, is outside what it was built for.

## Options considered

### The button

**A - R2 alone.** The conventional fast-forward button on handhelds. Rejected
by fact 3: it would take a working turbo away from every GBA game.

**B - SELECT as a modifier.** Rejected because SELECT is a game's button. Either
the game sees it and a chord also opens the map or switches the weapon, or
every SELECT press is held back until release, which delays it in every game
and makes holding it impossible. The display chords removed on 2026-09-30 took
the first road and hid L1, R1 and A from games whenever SELECT was held.

**C - MENU as a modifier, MENU+R1.** Nothing is lost: R1 is hidden from the
core only while MENU is held, and MENU means nothing to any game. The cost is
that MENU must open the menu on RELEASE instead of on press, or it could never
be a modifier. A tap is about a tenth of a second, so the menu opens that much
later. R1 over R2 is Eric's, for the feel in the hand.

**D - a Speed row in the in-game menu only.** Discoverable, and no button at
all, but the game stops to change it, which defeats flicking it on through a
slow stretch. Not rejected so much as not enough: a row can be added later,
and would arrive as a `SETSPEED` the launcher writes.

**E - the binding as launcher data, like turbo (ADR-0028).** Consistent with
how button meanings travel today. Rejected for now because there is one hotkey,
and a table for one entry is ceremony. Revisit at the second.

### Drawing

**Every refresh.** Fact 2 caps SNES under 2x and Genesis and GBA near 2.4x, so
the 3x and 4x steps would mean almost nothing on half the systems.

**Every other refresh while fast.** The picture changes 30 times a second
instead of 60, which is hard to see when everything is racing past, and the
core gets about 25 ms of every 33. Worked out from facts 1 and 2, not yet
measured: SNES about 2.7x, Genesis and GBA about 3.4x, NGPC 3.6x, NES and
TurboGrafx-16 the full 4x. Contained in the frame loop; 1x is untouched.

**Scale on another core.** The Brick has four. The loop would hand the frame
over and the scaling would leave its thread, which would let SNES near 3x while
drawing every refresh. But it changes how every frame is drawn, at 1x too,
inside ADR-0013's design. Too large for this, and kept as the revisit below.

### Sound

**Sped up.** Cheap through the existing resampler, but 4x is two octaves of
chipmunk, and the queue and its controller would need reworking for a rate
they were never given. Keeping pitch while speeding up needs real DSP on a CPU
that fast forward has already spent. **Slices** at normal pitch are choppy.
**Muted** keeps the queue fed and the controller blind to the change.

## Decision

**Diatom owns this one in-game hotkey: MENU+R1 steps the speed 1x, 2x, 3x, 4x
and back to 1x.**

- **MENU becomes a modifier.** Its press arms; its release opens the menu as
  the press does today, unless R1 was pressed while it was held. A launcher's
  pause request is unchanged.
- **R1 is hidden from the core whenever MENU is down**, so there is no first
  frame to leak, and stays hidden after MENU is let go until R1 is released
  too, through the same latch the menu's resume uses. The chord reads the
  physical pad (ADR-0019), so no remap can move it.
- **The step is a cap, and the frame loop meets it with a budget.** While fast,
  one turn of the loop covers two pacing periods: it runs core frames until it
  has run `2 x step` or until the next one would not fit in two periods less
  the measured present cost less a margin, presents the last, and advances the
  pacing clock by two periods. The present cost is the running average the
  exit summary already keeps, so a cheaper display mode buys more speed by
  itself. Every core frame still runs `diatom_cheevos_frame`, so achievements
  are judged on every frame the game runs, not every frame shown.
- **Audio is muted while fast.** The core's samples are discarded, as warmup's
  are, and each turn pushes two periods of silence, so the queue holds its
  target and returning to 1x needs nothing.
- **Speed is a state on ADR-0020's plane, owned by Diatom alone.** `SPEED x=N`
  is the reply to a `SPEED` query and is sent unsolicited on every change.
  Nothing writes it yet. It resets to 1x at every `RUN` and survives the
  in-game menu, since the player chose it. The launcher renders the toast from
  it and sends it back as an overlay (ADR-0027); Diatom draws no text.
- **`READY proto=7`**, for the reason proto 4, 5 and 6 bumped. Additive, so an
  old launcher ignores `SPEED` (ADR-0009); the number is for a new one, which
  should be able to tell whether the Diatom it has will answer MENU+R1 before
  telling a player it does.
- **The exit summary separates the two speeds**, as it already separates time
  paused: frames run fast, and how long, so the rate line still reads as 1x.

## Consequences

Easier: fast forward on every system with no button lost and no setting to
find; a second hotkey has an obvious home beside this one (MENU+L1, MENU+R2,
MENU+L2), and L2's TurboGrafx-16 Mode Switch stops mattering under MENU.

Harder: the menu opens on release, which every player feels a little and
nothing documents yet. Fast forward is silent, and SNES never reaches 3x. The
toast costs frames of its own today: Eric saw a game slow while one was up,
and that Sonic session logged 24 resyncs in 9,110 frames, against 1 in 4,192
for an earlier session of the same game in the same mode. Suspected, not yet
measured: `draw_overlay` reads the framebuffer back for every pixel it blends.
That cost has to be measured, and likely fixed, before a speed toast is drawn
over a loop already spending its budget.

Forecloses nothing: Option D and Option E stay open, and so does rewind.

## Revisit if

- The present cost drops below about 3 ms a frame - by scaling on another core
  or otherwise - which would make drawing every refresh affordable while fast.
- A second in-game hotkey is wanted: then the bindings become launcher data
  (Option E), and this one moves with them.
- Measured in Diatom, a step's real speed on a core falls below the step under
  it (3x reaching no more than 2x), which would make that step meaningless there.
