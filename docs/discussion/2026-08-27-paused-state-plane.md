# The paused loop dropped half the state plane

Reported by the PlayOS launcher, 2026-08-27, with a device log rather than a
description. Fixed in Diatom; nothing changed on the launcher side.

## Symptom, from outside

PlayOS moved its display-mode row from the shelf menu into the in-game menu,
which is the sensible place for it: a player can judge a mode against the game
instead of quitting, changing it blind and relaunching. The row sent
`SETDISPLAY\tmode=<name>` while paused and read the `DISPLAY` reply back.

The label changed. The game did not. The new mode appeared only after a quit
and relaunch.

## What the log showed

Every `diatom: display` line paired with a launch, two per launch, never one
from a menu:

    zip: .../Guerrilla War (USA).zip -> ...
    state: restored ...
    diatom: display stretch  nearest 1024x768 at 0,0     <- apply_display(sn->mode)
    diatom: warmup 3 frames in 37.1 ms
    diatom: display stretch  nearest 1024x768 at 0,0     <- the launch SETDISPLAY
    diatom: 435 frames in 8.07s

The launch `SETDISPLAY` worked because it lands in the running loop. The menu
one did not. The restart appearing to fix it was the setting having been
persisted and reapplied through the working path on the next launch.

## Cause

`menu_pause` enumerated five of the seven state-plane messages by hand -
`INPUTS`, `MAP`, `SETMAP`, `LEVELS`, `SETLEVEL` - and let everything else reach
`default: break`. `DISPLAY` and `SETDISPLAY` parsed correctly, arrived, matched
nothing, and were discarded with no reply and no log line.

`state_plane_msg` itself always handled all seven. Only the routing was short,
which is why this looked from the launcher exactly like a message never sent.

## Why it is worth more than the two lines

ADR-0020 says either side may read or write the state plane whenever it likes,
and that "the two moments it is drawing - menu and idle - are exactly the
moments Diatom is not". ADR-0022 put display mode on that plane. So display
mode was the one state-plane setting unreachable at the one moment a launcher
has a menu open to reach it from. Levels, input mapping and core options all
worked there. This did not.

The two accepted ADRs described behavior the code did not have, and nothing
caught it.

## Fix

The paused loop now ends in `default: state_plane_msg(&m); break;`, which is
what the running loop always did. `state_plane_msg` returns false for anything
it does not own, so this discards precisely what the old default discarded.

**The class, not the instance.** Adding the two missing cases would have
restored a hand-kept list that goes stale the next time something joins the
plane - which is the failure that just happened. The running loop's dispatch
could not go stale, and now neither can this one.

## What let it live

`test/stateplane.py` covers `DISPLAY` and `SETDISPLAY` while running, and
covers pause. It never crosses them. The suite passed throughout.

Pause is entered by a MENU keypress, so no headless driver can reach
`menu_pause` at all; testing it would mean an input hook in the runtime whose
only caller is a test. Recorded as open in register §13 rather than built,
because the structural fix above is a stronger guarantee than the test would
have been - but the reachability gap is real and will block the next thing that
needs proving in that loop.

## Credit where it belongs

The launcher session diagnosed this from the outside, correctly, down to the
routing, and deliberately did **not** work around it by re-sending `SETDISPLAY`
after `RESUME` - which would have applied the mode when the menu closed rather
than while it was open, and would have outlived the bug it papered over. That
is the right call and it is why the fix is two lines instead of a protocol
wart.
