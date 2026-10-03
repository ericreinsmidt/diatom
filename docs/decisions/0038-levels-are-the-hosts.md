# 0038. Levels are the host's; the port is the hardware under them

- **Status:** Accepted
- **Date:** 2026-10-02 (accepted the same day)
- **Supersedes:** -
- **Superseded by:** -

Amends [ADR-0020](0020-shared-state-plane.md), which put the levels in the
port, and holds the line [ADR-0007](0007-port-interface.md) drew: the port
deals in pixels, samples, buttons and time.

## Context

ADR-0020 made volume and brightness a state the launcher and Diatom share,
and the port implemented it whole: the position, stepping it from the
device's keys, the rescale onto the launcher's scale, reading the hardware
back. With one device port that was one copy.

With two it was two. On 2026-10-02 the Brick and GKD Pixel 2 ports shared
sixteen function names. Some were rightly the port's - the mixer register,
the backlight, the headphone jack - but `rescale` was byte for byte the same
in both, the window arithmetic was one formula with different numbers, the
step-and-clamp had the same shape, and hold-to-repeat, added that morning,
was the same state machine written twice. None of it is pixels, samples,
buttons or time. Eric asked whether the separation rule had been followed,
and it had not.

## Options considered

1. **Leave it.** Two copies, kept in step by hand. Every change to how a
   level behaves made twice, and the day they drift is a day one device steps
   differently from the other - the thing the button rules exist to prevent.
2. **A shared header for the ports**, like `port_clock.h`. Removes the copies
   but leaves the policy in the ports, still outside ADR-0007's line.
3. **The level in the host, the hardware in the port.**

## Decision

Option 3, with a shared header for the arithmetic only.

`src/levels.c` owns each level's position, forgets it at every handover,
sets it from the launcher (with ADR-0020's rescale), steps it from the level
keys, repeats a held key at the launcher's pace (300 ms, then every 90), and
asks the port to show the bar on each step.

The port interface for levels is the hardware: `diatom_port_level_positions`
(how many on this device, 0 for none), `_read` (the hardware as a position),
`_write` (put it there), `_keys` (which level keys are held, or pressed since
the last call), `_shown` (draw the bar), and `_invalidate` (forget the jack
state and the last write). What position 0 means, the launcher's mute
(ADR-0031) and re-mapping on a jack change stay in the port, because they are
about the device: the Brick cuts with its speaker switch and headphone
control, the Pixel 2 writes the register to 0. Which physical key is which
stays there too - on the Pixel 2 a volume key pressed with MENU held is a
brightness key until it is let go (ADR-0037).

`port/ladder.h` holds the window arithmetic and the nearest-rung lookup, one
copy, each port supplying its own numbers.

## Consequences

- One copy of every level rule. A change to stepping or repeating is made
  once and lands on every device.
- The port interface for levels grew from three functions to six, all of them
  plainly hardware. The protocol is unchanged.
- The port no longer acts on a level key itself. A tap shorter than a frame
  still counts because the port latches presses until the host reads them; a
  release the port never saw cannot leave the host repeating, because a held
  key is checked against the device before it is reported.
- The port keeps the position it last read or wrote, for the mute and the jack
  re-apply. That is a memory of the hardware, not the level, and invalidate
  clears it at every handover as before.
- Checked on the Brick and the Pixel 2: presses, holds, quick taps, both
  brightness schemes, the menu round trip, headphones mid-game, and on the
  Brick the mute switch before any volume press.

## Revisit if

A device's level does not fit "a number of positions the host steps through":
a continuous control, or one whose positions depend on something the host
cannot see. That would be the first level rule the port genuinely needs to
own.
