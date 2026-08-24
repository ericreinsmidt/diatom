# 0014. Offer six display modes; default to stretch

- **Status:** Accepted
- **Date:** 2026-08-24
- **Supersedes:** -
- **Superseded by:** -

## Context

Integer-scale-only was the working rule from the start (register §5), never an
argued decision. The first real game on the Brick showed it plainly: NES at 3x
is 768x720 inside a 1024x768 panel, so about a quarter of the screen is unused.
That prompted the question this record answers.

**Measured on the device, Contra on FCEUmm, firmware 1.1.1, 2026-08-24.** Six
geometries crossed with two filters, cycled live on the panel across two
sessions totalling roughly 20 minutes of play:

| mode | rect on a 1024x768 panel | nearest | sharp |
|---|---|---|---|
| native | 256x240 | 0.72 ms | 0.70 ms |
| integer | 768x720 | 5.96 ms | 5.94 ms |
| aspect | 936x768 | 7.73 ms | **14.98 ms** |
| fill | 1024x840, 72 lines cropped | 8.46 ms | 10.46 ms |
| stretch | 1024x768 | 8.42 ms | 10.65 ms |
| overscale | 1024x960, 192 lines cropped | 8.42 ms | 8.41 ms |

Everything held 60.08 fps against a 60.0998 target. **The only combination that
ever dropped a frame was `aspect sharp`, with 2 resyncs**, and it is also the
only combination over 11 ms.

Three facts came out of building this that argument would not have produced:

1. **FCEUmm reports an 8:7 pixel aspect (1.2190), not 4:3.** The prediction that
   fit, fill and stretch would collapse to one rect on a 4:3 panel was wrong;
   they are three different pictures.
2. **Sharp filtering is free at whole factors.** It is a no-op there by
   construction, and once the map builder collapses all-trivial weights it is a
   no-op in cost too: `integer` and `overscale` measure identically with either
   filter.
3. **`aspect` is the cheapest geometry in nearest and the most expensive in
   sharp.** It is the only mode where both axes are fractional. Every fullscreen
   mode is exactly 4.0 horizontally, so its horizontal axis collapses and only
   the vertical blends. Counterintuitive, and it inverts which mode is
   affordable depending on the filter.

## Options considered

### Option A - keep integer-only
Sharpest possible picture, every pixel a uniform 3x3 block, and the cheapest
fullscreen-capable mode at 5.96 ms. Rejected on the panel: about 25% of the
screen sits unused on a handheld whose screen is its whole interface.

### Option B - overscale (next whole factor, cropped)
Fills the panel with perfectly uniform 4x4 pixels and no filtering cost. The
crop is the problem: 192 of 960 lines, 24 source lines, enough to take a HUD.

### Option C - aspect-fit
Honours the shape the core asks for. Leaves 44px bars either side, and is the
one combination that dropped frames when filtered.

### Option D - stretch
Fills both axes exactly. On this panel that is a 9.4% horizontal stretch
(1.333 panel against 1.219 content), which was judged acceptable by eye in
motion. No crop, so nothing can be lost off the edges.

## Decision

**Six modes ship, all user-selectable. The default is `stretch` with `nearest`
filtering.**

The mode set is `native`, `integer`, `aspect`, `fill`, `stretch`, `overscale`,
selected by `--display`, with `--filter nearest|sharp` as an independent axis.

`stretch` because a handheld's screen is its entire interface and full use of it
beat both the letterbox and the crop when judged in motion on the real panel.
`nearest` because sharp earned no visible keep at these factors, costs 2 ms
where it is not free, and is the only thing that has ever made this frontend
miss a frame.

**Scope of the judgement, stated plainly:** this was decided on a 4:3 panel
showing near-4:3 content, where stretch distorts by 9.4%. The same rule on a
16:9 panel would distort by 78%. The default is a constant, not a computation,
and it is correct for the devices in scope today and not obviously correct
beyond them.

**Selection is Diatom's; presentation is not.** Diatom exposes the modes and
nothing more. Remembering a preference, offering a menu, or setting it per game
belongs to the host application (ADR-0009); PlayOS will pass `--display`, or its
protocol equivalent once that exists.

## Consequences

**Easier:** The picture uses the whole panel by default, which is what a handheld
wants. Every mode a user might ask for exists and is one flag away. The
arithmetic lives in `src/scale.c`, so a new mode is a host change that every port
gets for free - proven when six modes landed with zero changes to the desktop
backend's geometry handling.

**Harder:** Six modes is six things to keep working, and the filter doubles the
matrix. Two axes now need testing per port rather than one.

**Accepted cost:** A 9.4% horizontal distortion by default, deliberately, in
exchange for the full panel. Anyone who wants the core's intended shape has
`--display aspect` and pays 44px bars for it.

**Not decided here:** who owns the hotkeys. The on-device chords
(SELECT+shoulders, SELECT+A) exist because cycling needed to be possible while
looking at the panel. Whether a shipped Diatom keeps them, or whether all hotkeys
belong to the host application, is still open in register §9.

## Revisit if

- a target device's panel aspect differs materially from its content aspect,
  where the 9.4% that was acceptable here becomes something else entirely; or
- a core reports a pixel aspect far from square, making stretch's distortion
  much larger on the same panel; or
- a heavier core makes the 8.42 ms fullscreen blit unaffordable, at which point
  the disp2 hardware scaler (ADR-0013) stops being optional; or
- someone actually asks for a filter, which nobody has yet.
