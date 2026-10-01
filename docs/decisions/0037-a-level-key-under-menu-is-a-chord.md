# 0037. A level key pressed while MENU is held is a chord

- **Status:** Accepted
- **Date:** 2026-10-01 (accepted the same day)
- **Supersedes:** -
- **Superseded by:** -

Extends the chord rule of [ADR-0034](0034-fast-forward-on-a-menu-chord.md):
MENU is a modifier, armed by its press, acting on its release, and a release
that ended a chord does not open the menu.

## Context

The Brick has dedicated brightness keys, which its port handles like the
volume keys ([ADR-0020](0020-shared-state-plane.md)'s levels). The GKD Pixel 2
has none: its keys are the d-pad, A B X Y, L1 R1 L2 R2, SELECT, START, a
FUNCTION key (MENU on this device), volume up and down, and power. Eric wants
brightness adjustable during a game, on MENU plus the volume keys.

The port can see MENU held when a volume key goes down, and can change
brightness instead of volume. What it cannot do is tell the host that MENU's
release ended a chord: the port does not know the protocol or the host's
policy (ADR-0007), and MENU's meaning is host policy. Without that, letting go
of MENU after adjusting brightness opens the launcher's menu.

The host already reads every level every frame (`levels_tick`) to report
changes to the launcher.

## Options considered

### Option A - the host counts a level change under MENU as a chord (chosen)
If any level moves while MENU is held, the host marks the press chorded,
exactly as MENU+R1 does. Device-neutral: on the Brick nothing changes, because
its brightness keys do not involve MENU. One condition in the host, nothing in
the port interface.

### Option B - the port withholds MENU until it knows what MENU was for
Report MENU only on release, and only if no volume key came in between. Keeps
the host untouched, but moves policy into the port (what MENU means), delays
MENU+R1's arming, and breaks the rule that the port reports MENU like any
other button.

### Option C - no brightness during a game on this device
Set it in the launcher only. No code, and it is the honest fallback, but Eric
asked for it.

## Decision

Option A. The host marks MENU chorded when a level changes while MENU is held.
The Pixel 2 port maps MENU + volume up/down to brightness up/down, and plain
volume keys to volume.

## Consequences

- MENU plus a volume key never opens the menu on release, on any device. On
  the Brick that combination meant volume with MENU held, and now also does not
  open the menu afterwards - which is what a player holding MENU to adjust
  something would expect anyway.
- The rule keys off the level changing, not the key: a press at the end of the
  range moves nothing, so it does not count as a chord. A player pressing
  MENU + volume up at full brightness and letting go gets the menu. Accepted:
  rare, and visible.

## Revisit if

- A device gains a MENU combination that changes a level without the player
  meaning a chord, measured as menus failing to open.
