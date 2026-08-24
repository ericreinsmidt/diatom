# 0015. Add integer-vertical; keep stretch as the default

- **Status:** Accepted
- **Date:** 2026-08-24
- **Supersedes:** -
- **Superseded by:** -

Extends [ADR-0014](0014-display-modes-and-default.md), which stands unchanged.
Its default is untouched; this adds a seventh mode and records why the obvious
alternative default was rejected.

## Context

After ADR-0014 landed, the reasonable question came up: should the default be
the largest whole factor that cuts nothing, accepting pillarbox or letterbox,
on the grounds that a safe default is better than a pretty one when every mode
is offered anyway?

The premise is that integer scaling is the undistorted option. **Measured on
the device, 2026-08-24, all six test-matrix cores on a 1024x768 panel:**

| System | Reported aspect | Integer rect | Shape error, integer | Shape error, stretch |
|---|---|---|---|---|
| Game Boy | 1.1111 | 800x720 | exact | +20.0% |
| GBA | 1.5000 | 960x640 | exact | -11.1% |
| Genesis | 1.3333 | 960x720 | exact | exact |
| NES | 1.2190 | 768x720 | **-12.5%** | +9.4% |
| SNES | 1.3333 | 768x672 | **-14.3%** | exact |
| PC Engine | 1.2000 | 768x729 | **-12.2%** | +11.1% |

**Integer scaling preserves source pixels, not intended shape, and for half
this matrix those are not the same thing.** It is exact for the systems whose
pixels were square - Game Boy 160x144, GBA 240x160, Genesis 320x240 - and wrong
by 12-14% for the CRT-era systems that reported a pixel aspect their resolution
does not imply. For NES and PC Engine, integer distorts *more* than stretch
does, in the opposite direction; for SNES it is 14.3% wrong where stretch is
exact.

Screen used by integer ranges from 66% (SNES) to 88% (Genesis).

## Decision

**The default stays `stretch`.** The case for integer rested on it being the
undistorted choice, and it is not. ADR-0014's default was chosen with eyes on
the panel across two sessions; a table is weaker evidence than that, and this
table does not even support the change.

**A seventh mode, `integer-vertical`, is added:** a whole factor vertically,
shape-correct horizontally. It is the honest version of what "safe integer
scaling" was reaching for.

Measured, matching prediction exactly:

| System | integer | integer-vertical |
|---|---|---|
| NES | 768x720 | **878x720** |
| SNES | 768x672 | **896x672** |
| PC Engine | 768x729 | **875x729** |
| Game Boy | 800x720 | 800x720 |
| GBA | 960x640 | 960x640 |
| Genesis | 960x720 | 960x720 |

For square-pixel content it collapses to exactly what plain integer gives, so
**it is never the worse of the two**. For the rest it corrects the shape and
uses more of the panel, paying with a fractional horizontal factor.

## Consequences

**Easier:** Anyone wanting uniform pixels without the shape being wrong now has
a mode that delivers it. The axis where uniformity reads most strongly stays
exactly whole.

**Harder:** Seven modes, fourteen combinations with the filter. The set is now
at the edge of what is worth carrying, and an eighth needs a real argument.

**Recorded because it will otherwise be re-argued:** "integer scaling means no
distortion" is intuitive, widely repeated, and false for any system with
non-square pixels. The numbers above are the answer next time.

## Revisit if

- an eighth mode is proposed, at which point ask what it does that seven do not;
  or
- a core reports an aspect so far from its resolution that integer-vertical's
  horizontal factor becomes extreme.
