# The exit flash: four fixes, still there, and what is ruled out

Written 2026-08-27, mid-investigation, because the session that holds this
context is nearly full. **The bug is NOT fixed.** This is the state of it.

> **RESOLVED later the same day, in PlayOS.** See *The answer* at the end. The
> body below is left exactly as written, wrong guesses included, because the
> prediction it ends on turned out to be right and that is the useful part.

## Symptom

Quitting a game flashes: a visible discontinuity between the in-game menu and
the shelf. Reported consistently on quit, and never on opening the in-game
menu. It always settles on the correct picture and never wedges.

## Where it now stands

**Diatom does nothing at all at the quit handover, and the flash persists.**
That is the single most important fact here, and it was measured rather than
argued: with the menu open and the player quitting, Diatom performs zero pans.

    after the menu opened: 0 pans to the park page, through the quit

Diatom is paused from `PAUSED` onward - not presenting, not panning, not
touching `/dev/fb0`. So whatever flashes happens between the launcher's own
menu frames and its own shelf frames. **That makes it PlayOS's, not Diatom's**,
unless something below overturns it.

## What the four Diatom changes did, and why none of them was it

Each was a real defect and each is worth keeping. None was the flash.

**1. `present_stop`, draining the flip thread (`8abb076`).** `EXIT` was sent
while the flip thread still had a pan in flight, so for a few milliseconds two
processes drove one framebuffer. Real, closed. Also replaced a 20ms
`nanosleep` before `PAUSED` that was standing in for the same wait.

**2. Parking on page 2 (`f8469ff`).** The launcher renders into pages 0 and 1;
Diatom used 0, 1 and 2. Stopping on 0 or 1 left the panel scanning out a page
the launcher was about to draw into. Real, closed.

**3. Park black at exit, keep the frame at pause (`95f07ab`).** Parking a copy
of the last game frame at exit replaced the launcher's menu with a bare game
frame. Real, closed - and the fix that finally made the quit-only asymmetry
make sense.

**4. Do not park when something else owns the display (`fba030f`).** The one I
caused. Quitting from the menu leaves Diatom paused and presenting nothing, so
parking did not hand the display over, it TOOK it - 150ms of black seized from
a live menu:

    1.667  0,768     launcher drawing its menu
    1.750  0,1536    Diatom seizing the panel to show black
    1.900  0,0       launcher getting it back

That was a large flash of my own making, layered on top of the original one.
Removing it is what makes the current measurement meaningful.

**The pattern worth not repeating:** three of those four asked *what to park*
and never *whether to park*. "Still flashes" was reported after each, and each
time it was read as a new symptom rather than as the same one plus a new one.

## Ruled out, with evidence

From the launcher side (measured by the PlayOS session):

- **The launcher page-flips.** 300 samples of `/sys/class/graphics/fb0/pan`
  while the shelf presented: 151 at `0,0`, 149 at `0,768`, nothing else. It is
  double buffered and does not composite into the front buffer.
- **Vsync works.** `playos.elf` idles at 6.2% CPU in state S with the system
  89.5% idle - it blocks on the swap rather than spinning.
- **Opaque pixels.** A launcher frame is alpha 255 in all 786432 pixels, so the
  per-pixel-alpha invisibility trap is not in play.
- **One buffer, no third party.** Pages 0 and 1 are the launcher's, page 2 is
  Diatom's park, and the shelf lands in `fb0` itself rather than a composited
  layer.
- **Clearing the launcher's back buffers before the fade did nothing.** Tried
  and reverted on that side: a swap pans to the buffer just rendered into, so a
  page is only displayed after being drawn.

From this side:

- **Diatom is inert at the quit handover** (above).
- **No `pan to park page failed` in any log.**
- **Park contents are correct**: page 2 holds 92688 non-black pixels at
  `PAUSED` and exactly 0 after `EXIT`, alpha 255 in both.

## The lead nobody has followed

A 215ms gap with no pan changes at all, immediately after the quit:

    215 ms at t=1.880

Diatom is not panning then, so the panel is holding one of the launcher's own
pages for a fifth of a second. Whatever that page contains is what the user
stares at across the transition. That is the most specific unexplained
observation in this whole investigation and it is on the launcher side:
between `EXIT` and the first fade frame, PlayOS invalidates textures, calls
`prime_window`, and then runs `anim_return`. If the flash is a held frame
rather than a torn one, that gap is where it lives.

## Instruments

- `tools/panwatch.c` - reads the pan offset in a tight loop and prints only
  changes, with microsecond timestamps. Written for this. Its limitation
  matters: Diatom and the launcher share offsets 0 and 768, so only `1536` is
  unambiguously Diatom's.
- `dd if=/dev/fb0 bs=4096 skip=1536 count=768` reads park page 2. Note
  `skip` is in 4096-byte blocks and a page is 768 of them - getting that wrong
  reads the launcher's page 1 and looks like the park never happened.
- `tools/keyinject.c` - A=305, B=304, MENU=316, brightness FN keys 317/318,
  d-pad `hat`/`hatx`. Never inject `KEY_POWER` (116): the kernel and stock
  firmware see it too and the device powers off.

## If picking this up

The next measurement is a 240fps capture of a quit with the current build
(`69cf7b72` or later), classified per row against a known menu frame and a
known shelf frame. Diatom is provably out of the picture, so that capture is
now measuring the launcher alone: whether the flash is a torn frame, a held
frame, or a black gap during that 215ms.

Do not add another park variant to Diatom without evidence that Diatom pans at
the moment of the flash. It does not, currently.

---

## The answer, 2026-08-27

**It was PlayOS, and it was a double present.** `anim_return` called `render()`,
which ends in `SDL_RenderPresent`, and then presented again after drawing its
fade overlay. Two presents per frame for the length of the fade, so the panel
alternated bright and dim throughout it. Found and fixed on the launcher side.

**The conclusion this document ends on was correct**, and it was correct
*before* anyone knew the cause: with Diatom performing zero pans through the
quit, the flash had to lie between the launcher's own frames. Everything
downstream of that measurement held up. What the four Diatom changes cost was
real, but the reasoning that ruled Diatom out was sound and it is what pointed
at the right file in the end.

**Two things this got right that are worth keeping:**

- **Measuring pans rather than arguing about parking.** Three of the four
  changes asked *what to park* when the answerable question was *whether Diatom
  moved the panel at all*. Once that was instrumented the search space
  collapsed. The lesson is the one already in the register: an unfalsifiable
  claim costs more than a slow measurement.
- **Not adding a fifth park variant.** This session declined to guess again
  without evidence that Diatom pans during the flash. That evidence never
  arrived, because there was none to find.

**One thing it got wrong.** The 150 ms black seizure removed by `fba030f` was
introduced by an earlier fix in this same hunt. While it was present, every
"still flashes" report was measuring two faults stacked, and the reports could
not distinguish them. Fixing forward on a symptom that several people are
observing at once needs the intermediate states labelled, or the feedback is
noise.

**The unfollowed lead was a red herring.** The 215 ms gap with no pan changes
was the launcher holding a frame during its own fade, which is exactly what the
double present was doing. It pointed the right way and was never followed here.

All four Diatom changes stand. Each fixed a real defect at the handover, none
of them was this, and none of them is reverted.
