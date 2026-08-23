# 0003. Digital-only input — no analog support

- **Status:** Accepted
- **Date:** 2026-08-23
- **Supersedes:** —
- **Superseded by:** —

## Context

libretro distinguishes `RETRO_DEVICE_JOYPAD` (digital buttons and a d-pad) from
`RETRO_DEVICE_ANALOG` (axis pairs). A frontend may implement either or both.

Facts bearing on the choice:

- **Curatorial intent.** Eric does not want to play games that require
  thumbsticks. This is a taste decision, stated 2026-08-23, and it is the
  primary driver.
- **Hardware.** The Miniloong Pocket 1 *has* analog — measured 2026-08-22:
  `ABS_X`, `ABS_Y`, `ABS_HAT0X`, `ABS_HAT0Y`, `BTN_THUMBL`, across two
  `Loong Gamepad` nodes. The Brick is believed to lack sticks but this is
  **unverified**. So the decision is not forced by hardware — the Miniloong
  could support analog if we wanted it.
- **Button count is not a constraint.** Both devices provide four face and four
  shoulder buttons. Six is the maximum any in-scope system asks for (SNES,
  6-button Genesis, CPS2), and the standard four-face-plus-two-shoulder mapping
  covers it — the same layout SNES itself uses. An earlier claim in discussion
  that six-button games map "badly" onto shoulders was wrong.
- **The systems that genuinely need analog** — N64, DualShock-era PS1, DS, PSP,
  3D Saturn/Dreamcast — sit at or beyond the target class's CPU ceiling anyway
  (§0b). Excluding analog costs little that was reachable.

## Options considered

### Option A — full analog support
Maximally general. Costs deadzone handling, calibration, axis-to-button mapping,
per-core analog configuration, and the UI to expose it. Serves zero curated
games. By the §0 seam test this is an abstraction with no implementation behind
it — a tax.

### Option B — digital only
Input state reduces to a button bitfield plus a hat. The port interface stays
small. Nothing that is actually going to be played is lost.

### Option C — digital only, but pre-shape the API for analog
Middle ground. Rejected as a *feature*, but partly retained as a constraint:
we will not implement analog, and we will also not design the input path so
that adding axes later requires tearing it up. Not building something is
different from actively foreclosing it.

## Decision

**Option B.** The frontend implements digital input only. No analog axes, no
`RETRO_DEVICE_ANALOG`, no calibration or deadzone machinery.

Per Option C, the input path should not be *gratuitously* hostile to a future
axis, but no abstraction is built in anticipation of one.

## Consequences

**Easier:** Input state is a bitfield. No deadzones, calibration, axis mapping,
or the config surface those drag in. The port's input interface stays minimal,
which matters because it is one of the seams most likely to leak device
knowledge (§4). And since nothing needs sticks, the Brick's probable lack of
them stops mattering — **the two devices become input-equivalent**, which argues
for uniform curation and one test matrix rather than two (§8).

**Harder:** Adding analog later means revisiting the port interface, the input
path, and core device-type handling together. Any future target device whose
primary directional input is a stick rather than a d-pad is out.

**Foreclosed:** N64, DualShock-era PS1, DS, PSP, and analog-dependent games
generally. Note this does **not** by itself settle PS1 — much of the early PS1
library is d-pad-playable, and PS1's real cost is technical (CD images, BIOS,
CHD, large save states), which remains open in §0b.

**Explicitly NOT removed:** controller **device-type** selection. The frontend
must still call `retro_set_controller_port_device` — Genesis 3-button vs
6-button is a correctness issue (some early games misbehave with a 6-button pad
attached, which is why the real pad has a Mode switch), and PC Engine has the
same 2- vs 6-button split. Digital-only removes axes, not device types. This is
the easiest part of this decision to get wrong later.

## Revisit if

- A target device ships without a usable d-pad, making a stick the only
  directional input; or
- a firmware adopting this frontend wants a system whose library is materially
  d-pad-hostile; or
- the target class definition in §0b is widened to include 3D-era systems.
