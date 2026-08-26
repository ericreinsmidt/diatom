# Spike result - the backlight floor, and the launcher's ladder

- **Date:** 2026-08-26
- **Question:** *`port/brick.c` floors brightness at 8/255. Where is the panel's
  actual lower limit, before that number gets locked in?*
- **Status:** Answered, and redirected. **The panel's floor is raw 2**, and 8
  turns out to be defensible - but the floor was the smaller half of the
  question. **The scale's shape is wrong**, and the launcher already has the
  right one.
- **Instrument:** [`tools/dispprobe.c`](../../tools/dispprobe.c) (`--sweep` mode
  added for this)

## Why it was worth asking

The constant carried its own justification:

```c
#define BRIGHT_RAW_MIN 8    /* never fully dark - a black screen looks broken */
```

The comment argues that *a floor should exist*. It is not evidence that **8** is
the floor. 8 was chosen. That is the shape of claim this project has paid for
repeatedly, so it was worth an hour before it became something to preserve.

## The driver has no floor of its own

First question, and the one that costs nobody's attention: does the disp2 driver
clamp low values? If it did, the floor would be a hardware fact and the port
would only have to agree with it.

It does not. A write/read-back ladder of nineteen values returned every one
unchanged:

```
was = 160
  0 -> 0      4 -> 4      8 -> 8     20 -> 20    38 -> 38
  1 -> 1      5 -> 5     10 -> 10    26 -> 26    64 -> 64
  2 -> 2      6 -> 6     13 -> 13    32 -> 32   128 -> 128
  3 -> 3      7 -> 7     16 -> 16               255 -> 255
restored = 160
```

**Nothing below the port prevents a true 0.** So a floor in `brick.c` is
load-bearing rather than decorative, and no ioctl can tell us where to put it.
It needs eyes.

Read-back is also, therefore, close to an echo. It reflects driver state - it
later caught two changes this session did not make - but it is not a measurement
of light. **Nothing except a person looking at the panel verified brightness at
any point in this spike**, and that is worth stating plainly given how much of
the session's output is numbers.

## The floor is raw 2

Descending bisection from 255 reported every value visible, including raw 1.
A direct re-test from the bottom disagreed:

| raw | descending bisection | direct, from below | verdict |
|---|---|---|---|
| 0 | black | black, twice | **black** |
| 1 | "lit" | black, then visible, then black | **black** |
| 2 | not tested | visible, twice | **first visible step** |

**The bisection was biased and the method is the finding.** Walking down from
255 dark-adapts the eye at every step, so the observer is more sensitive at each
new low than they were at the last - and "can I detect light" drifts underneath
the measurement. Approaching the same values from black removes that. Where a
threshold depends on the observer's state, sweep *toward* the adapted condition,
not away from it.

Note also that raw 2 is visible *after adaptation, in one room*. In daylight it
is indistinguishable from off. The panel's physical floor and the lowest setting
that still reads as "the device is on" are different numbers, and only the first
was measured here.

## The launcher's ladder, which answers the better question

PlayOS's brightness keys were pressed from maximum to minimum while the raw
value was polled. The full ladder, eleven levels:

| lvl | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| raw | 1 | 8 | 16 | 32 | 48 | 72 | 96 | 128 | 160 | 192 | 255 |
| step | | +7 | +8 | +16 | +16 | +24 | +24 | +32 | +32 | +32 | +63 |
| ratio | | 8.0x | 2.0x | 2.0x | 1.5x | 1.5x | 1.33x | 1.33x | 1.25x | 1.20x | 1.33x |

The differences grow from 7 to 63 while the ratios fall toward 1.2x. That is a
**geometric ladder**: roughly constant *proportional* change per step, which is
how brightness is perceived. It is hand-tuned rather than any clean curve.

Two things follow immediately.

**8 was a lucky guess, not a wrong one.** It is exactly PlayOS's level 1. The
comment's reasoning was sound and its number was unjustified; both can be true.

**Diatom's scale is the wrong shape.** It is twenty *linear* steps:

```
0 13 26 38 51 64 77 89 102 115 128 140 153 166 179 191 204 217 230 242 255
```

Nine of those twenty sit above raw 128, inside the launcher's top **two** levels,
where consecutive steps are close to indistinguishable. Meanwhile the entire
range from 1 to 12 - five of the launcher's eleven levels - is unreachable at any
setting, because level 0 clamps to 8 and level 1 is already 13.

That is also why lowering the floor alone would have been nearly pointless: it
changes exactly one setting. **The floor and the curve are one decision.**

## What this changes

- **The port's clamp becomes 2**, measured, guarding the true 0 that nothing
  below us prevents.
- **The linear ramp should become geometric.** Not because PlayOS does it, but
  because eight successive halvings of duty were all visible, and a linear scale
  spends half its steps where they cannot be told apart.
- **The exact ladder and step count are launcher policy, not port policy.**
  Hardcoding eleven PlayOS levels into `brick.c` would tie a standalone frontend
  to one firmware. This belongs with the level-sync item in the register.

## The register item this corrects

The open item read *"volume and brightness set in-game do not survive the exit"*.
The truth is stronger, and worth the rewrite: Diatom's twenty linear steps land
on values **the launcher has no level for**, so on resume brightness does not
merely get discarded, it snaps to PlayOS's nearest level. The reverse holds too -
Diatom reads the raw value on first press and divides it by its own scale, so it
begins from a level the launcher never set. The two sides do not disagree about
a *number*; they disagree about what the numbers *are*.

## Outcome, verified on hardware the same day

The twelve-rung ladder `2 4 8 16 32 48 72 96 128 160 192 255` replaced the
linear ramp and was tested in Contra. **The spacing reads as even**, which is
the result the geometric argument predicted and the linear scale could not
produce.

The bottom rung was reported as *not usable* for that game in a normally lit
room - and is **kept anyway**, deliberately. Two reasons:

- The floor exists to stop the screen going *black*, not to guarantee a
  comfortable picture. Raw 2 is visible, so it does its job. "Too dim for Contra
  right now" and "should not be reachable" are different claims, and only the
  first was observed.
- A very dim rung is a feature on a handheld in a dark room. Trimming the ladder
  back to the comfortable range would have discarded the measurement this spike
  was run to obtain, leaving the constant exactly as unjustified as before.

Recovery from the bottom rung is **demonstrated rather than assumed**. Polling
the raw value while the keys were pressed caught the ladder being walked off the
floor and back six times:

```
2 4 2 4 2 4 2 4 8 4 2 4 8 4 2
```

The OSD bar is near-white and is the brightest thing on the panel at raw 2, so
the control needed to climb out is the last thing to disappear. That was the
original justification for a floor at all, and it is now evidence.
