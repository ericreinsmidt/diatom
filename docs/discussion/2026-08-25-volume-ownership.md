# Who owns the volume keys

The register said *"brightness and volume stay with the firmware"* and had it
ticked. `port/brick.c` says the same thing in a comment: *"volume is the host
OS's business. All unmapped."*

Both are assumptions, and PlayOS's own source contradicts them.

## The state today

**Nothing handles the volume keys while a Diatom game is running.** The guard
kills `playos.elf` and `minarch.elf` and freezes the supervisor, so the code
that reads `KEY_VOLUP`/`KEY_VOLDN` is not running. `trimui_inputd` survives -
it lives under `runtrimui.sh`, outside PlayOS's launch chain - but it delivers
the events rather than acting on them.

PlayOS handles them itself, in `minarch/overrides/all/common/api.c`:

    case KEY_VOLUP: { int v = GetVolume(); if (v < VOLUME_MAX) SetVolume(v + 1); } break;
    case KEY_VOLDN: { int v = GetVolume(); if (v > VOLUME_MIN) SetVolume(v - 1); } break;

and the file's header says why in one line:

> volume and brightness are applied in game, because nothing else does it

## The structural reason

**Whoever owns the input loop during a game has to handle the volume keys,
because nothing else sees them.** That holds regardless of who "owns" volume in
an ownership sense. minarch does it while believing volume belongs to the
firmware, and it is right to.

Diatom owns the input loop during a game by ADR-0008 and ADR-0009. So the work
lands here whether the concept belongs here or not.

## Three placements

| | Mechanism | Cost |
|---|---|---|
| **Launcher** | Diatom forwards `VOLUP`/`VOLDN` over the protocol; the launcher sets the gain | Volume dies with the launcher - and ADR-0008 exists precisely so a dead launcher cannot take the game down. Losing volume control to a crash is poor on a handheld |
| **Host** | Diatom acts on the keys and calls into the port | Puts a domain concept in the host, and the gain is per-device: the Brick's control is inverted and its driver advertises the wrong direction |
| **Port** | The port handles the keys internally and never reports them upward | Volume is hardware and ports do hardware ([ADR-0007](../decisions/0007-port-interface.md)). Keeps working when everything above is dead. The inversion trap stays in `brick.c` beside the other device quirks |

**The port is the recommendation.** It is the same reasoning that put the L3/R3
quirk there - a device fact that must not travel upward - and the same reasoning
PlayOS reaches from the other direction, since minarch draws the indicator
during a game rather than the launcher doing it.

## The OSD is not the obstacle it was claimed to be

An earlier version of this argument said Diatom cannot show feedback because the
on-screen overlay was deleted and there is no text rendering. **That is wrong.**
PlayOS's indicator has no text in it. `PLAYOS_drawSettingLine` is a scrim and
two filled rectangles:

| Element | Geometry | Colour |
|---|---|---|
| scrim | full width, `line_h + 2*pad` tall, at the very top | black, alpha 128 |
| track | full width, `line_h` | RGB(60, 62, 72) |
| fill | `W * pct`, `line_h` | RGB(235, 235, 240) |

`line_h` is `SCALE1(2)`, which is 6px at the Brick's FIXED_SCALE of 3; `pad` is
half that. Its own comment: *"No glyph, no number - you know which button you
just pressed."*

Diatom already blits rectangles, and `brick.c` already composites per-pixel
alpha for the framebuffer layer, so the scrim is an operation that exists.

Worth keeping the distinction PlayOS draws deliberately: `kind` 1 and 2 select
`GetBrightness()` or `GetVolume()`, and the drawing is identical **on purpose**,
so the same feedback reads the same on the shelf, in a game, and in the in-game
menu. They are not the same line; they are made to look like it.

## What stays open

Whether the **port** draws the bar or the **host** asks it to. "How full is the
bar" is policy and "put pixels on the panel" is hardware, so the clean split
would have the host compute a fraction and the port render it - which grows the
port interface, already at twelve functions against ADR-0007's ten.

Not settled here. Nothing is implemented yet, and settling it before there is
code to settle would be deciding without the information that makes it obvious.

## The device facts this needs

Recorded in §6 and in `tools/brick-device-run.sh`, both learned expensively:

- `digital volume` (0-63) is the speaker level and is **inverted** - lower is
  louder, 0 is loudest - while the driver advertises `step=+1.16dB`.
- `Headphone Volume` is **not** a speaker level. Raising it routes to the
  headphone jack and mutes the speakers. PlayOS zeroes it deliberately.

## Built 2026-08-25, and what it does not cover

The port owns it, as recommended. 20 steps of 5%, driven by a raw ioctl on
`/dev/snd/controlC0` (no alsa-lib in the sysroot, and forking `tinymix` per
keypress is a process spawn in the input path). Level 0 switches `HpSpeaker`
off, because `digital volume` advertises `mute=0` and its minimum is about
-74 dB rather than silence - audible with an ear against the speaker. Shutdown
hands the speaker back unconditionally.

**It is in-game only**, and that is correct rather than a limitation. Whoever
owns the input loop handles these keys because nothing else sees them, and
ownership alternates cleanly - the launcher has the display and the input
between games and during an in-game menu pause, so it handles volume then.
There is no window where neither does.

**What is not handled is that the two do not agree.** The launcher re-applies
its own stored level the moment its UI resumes, so a change made in-game is
silently discarded on exit. Shutdown deliberately does not restore the level -
it is a user setting - but that reasoning was incomplete, because *not*
restoring it is not the same as it persisting when something else overwrites it
a second later.

That fix is protocol-shaped rather than port-shaped, and belongs beside
ADR-0019's remap message: either the port reports the level upward for the
launcher to adopt, or the launcher supplies its level at startup. Tracked in §8.
