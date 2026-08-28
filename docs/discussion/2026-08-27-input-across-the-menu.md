# Input held across the menu, and the mask with two writers

Reported by the PlayOS launcher, 2026-08-27. Second Diatom bug found from
outside today, both in the paused/resume path.

## Symptom

Dismissing the in-game menu presses the button into the game. Choose Continue
with A and the game gets an A.

## Cause

`menu_pause` blocks in `diatom_proto_poll(&m, -1, false)` and never calls
`diatom_port_input_poll`, so every pad event from the launcher's menu session
queues up and lands in one drain at resume. `g_buttons` then describes the
player's fingers at that instant, and the button that dismissed the menu is
still down.

The resume path already re-read the pad into `prev_buttons`, with a comment
recording that this was measured with a human on 2026-08-25 after one press
produced two menus. That guard is real and it works - for everything
edge-triggered. MENU cannot reopen; the chord does not re-fire.

**The core is the one consumer that reads the level.** `cb_input_state` returns
`diatom_port_input_state() & ~g_suppress`. So the single guard in place missed
the single consumer that mattered, and it missed it because of the shape of the
consumer rather than by oversight.

## The part that was not in the report

The obvious fix - latch the held buttons into `diatom_env_suppress` at resume -
does not work, and would have looked like it did.

`display_chord` runs every frame and sets the mask **absolutely**, including
`diatom_env_suppress(0)` on any frame SELECT is not held. A latch written at
resume is cleared one frame later. The launcher spotted this from outside and
correctly declined to judge it.

So the fix is not the latch. It is that `g_suppress` had two writers with
different lifetimes:

- the chord mask, recomputed from the pad every frame
- the resume hold-off, latched until release

One variable, two lifetimes, and the one that runs every frame wins. That is
the defect; the stray A is a symptom.

`display_chord` now **returns** its mask and sets nothing. The loop composes
both and calls `diatom_env_suppress` once. A third reason to hide a button adds
a term instead of a race.

## Two judgment calls

**Suppress until release, not until the next press.** Narrowing the latch to
buttons that went down *while paused* is more precise-looking and has a hole:
release A during the menu, press it again to choose Continue, and it was held
before the menu too, so it escapes. The conservative reading costs holding a
direction across a menu, and that self-corrects on the next press. It also
matches what the MENU guard already does.

**Set the mask at the resume site, not only at the top of the loop.** The
resume path ends in `continue`, which goes straight to `retro_run`. Recording
the latch without applying it would leak the button on precisely the frame this
exists to protect.

## Why Diatom and not the launcher

The launcher could refuse to send RESUME while any button is held, and made the
better argument for not doing it: it only covers buttons the launcher maps, and
it delays the resume for as long as the finger stays down, which is visible.

The framing that settles it is theirs: this is a statement about Diatom's own
pause lifecycle - *on resume, do not deliver to the core input that arrived
while paused* - not about who owns the display.

## Unverified

The path is read from source. The mechanism it rests on was measured with a
human. The fix itself has not been watched to work, and the bug has not been
watched to happen, because `menu_pause` is reachable only by a keypress and the
device is in use.

**This is the second fault today in a region no test can reach.** The first was
the paused loop dropping half the state plane. Both were found by the launcher.
Register §13 argued that morning that a structural fix beat inventing a test;
that was right about that bug and wrong as policy, and §13 now says to build
the synthetic-input hook before the next change to `menu_pause`.
