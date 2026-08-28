# 0007. The port interface

- **Status:** Accepted
- **Date:** 2026-08-23
- **Supersedes:** -
- **Superseded by:** -

## Context

The host ↔ port boundary is the only load-bearing seam in Diatom
(§2). It has three real implementations - `desktop`, `tg5040`, `miniloong` - so
getting it wrong means rewriting all three. Everything else in the host is
internal organization that can be refactored freely.

Constraints already fixed by prior decisions and measurement:

- **ADR-0003** - digital input only, no analog axes.
- **ADR-0006** - cores are `dlopen`'d once and never unloaded.
- The Brick's panel is 1024×768; the Miniloong's framebuffer is **720×960
  portrait**, presenting as 960×720 landscape (measured 2026-08-22). Rotation is
  therefore required on one device and not the other.
- Both panels take clean integer scale factors for every in-scope system.
- Cores emit video and audio from inside `retro_run`, in an order that varies by
  core.

## Options considered

### Video: where the frame is consumed

**Option A - present synchronously inside the core's video callback.** No extra
buffer. Rejected: at the moment `video_refresh` fires, the host does not yet know
how much audio that frame produced, because audio callbacks may arrive before or
after video within the same `retro_run`. Presenting there means committing to a
frame before the information needed to pace it exists.

**Option B - copy into a host-owned buffer in the callback; present after
`retro_run` returns.** Costs one memcpy per frame. Measured against A53-class
memory bandwidth (order 1-3 GB/s) that is roughly 1%:

| | Bytes/frame | MB/s at 60Hz |
|---|---|---|
| GBA 240×160 | 76,800 | 4.6 |
| SNES 256×224 | 114,688 | 6.9 |
| Genesis 320×224 | 143,360 | 8.6 |
| SNES hires 512×448 | 458,752 | 27.5 |

### Audio: what the port must expose

**Option A - `queued()` only.** Insufficient. Dynamic rate control keeps the
buffer near *half full*; without capacity the host cannot compute the target.

**Option B - `queued()` plus capacity in caps, non-blocking writes.** A blocking
write is a legitimate sync strategy - the audio device paces everything - but it
is incompatible with DRC and makes video pacing jittery.

## Decision

**Ten functions. The port deals in pixels, samples, buttons and time. Nothing
else.**

```c
typedef enum { DT_PIX_RGB565, DT_PIX_XRGB8888 } dt_pixfmt;
typedef struct { int x, y, w, h; } dt_rect;

typedef struct {
    int  surface_w, surface_h;    /* logical, always landscape */
    int  audio_rate;              /* what the device actually runs at */
    int  audio_buffer_frames;     /* capacity - DRC needs this, not just queued */
    bool present_blocks;
} dt_port_caps;

bool dt_port_init(dt_port_caps *out);
void dt_port_shutdown(void);

/* Called AFTER retro_run returns, never from inside the video callback.
   dst is precomputed by the host. src == NULL repeats the previous frame. */
void dt_port_present(const void *src, int w, int h, size_t pitch,
                     dt_pixfmt fmt, dt_rect dst);

void   dt_port_audio_write(const int16_t *frames, size_t n);  /* never blocks */
size_t dt_port_audio_queued(void);

void     dt_port_input_poll(void);
uint32_t dt_port_input_state(void);   /* canonical Diatom buttons */

uint64_t dt_port_now_us(void);
void     dt_port_log(dt_log_level lvl, const char *msg);
```

**The port must never include `libretro.h`.** This is the concrete test for
whether the seam holds. If the port knows retropad IDs or libretro's pixel-format
enum, the boundary is splitting one program across files rather than separating
device from emulation - and a port that needs a core to build cannot be
developed on the desktop, which destroys the iteration loop.

Consequent rules:

- **Host computes `dst`; the port performs the blit.** Scaling arithmetic exists
  once, so every port behaves identically. Recompute on dimension change - SNES
  hires and PC Engine's variable width both change mid-game - not per frame.
- **`surface_w/h` are logical and always landscape.** The Miniloong's portrait
  framebuffer is invisible above the port, enforced by the type.
- **Buttons are a Diatom enum, not retropad.** The mapping will be near-identity;
  its purpose is keeping `libretro.h` out of the port. Device quirks such as the
  Brick reporting F1/F2 as L3/R3 terminate here.
- **Pixel formats pass through.** Accept `RGB565` and `XRGB8888`, refuse
  `0RGB1555`. An earlier proposal to force a single format on cores is rejected:
  both formats convert free on SDL texture upload, so forcing one would sometimes
  add a conversion that is otherwise gratis. No in-scope core needs 1555.
- **No paths in the port interface.** An earlier draft had `dt_port_path()` for
  save and BIOS directories. `save` and `system` are domain nouns - precisely the
  anti-pattern. Paths come from the **launcher**, which already knows the SD
  layout, and are handed to the host at startup.

## Defaults, not decisions

Recorded so they are visible as choices rather than accidents. Either may change
without superseding this ADR.

- **`SET_ROTATION`: decline.** Return false, and log when a core asks. Honouring
  it would require rotation in the port *on top of* panel rotation - doubling the
  trickiest code for a case that does not arise now arcade is out of scope
  (ADR-0005). The log is how we find out if that assumption is wrong.
- **When integer scale does not fit:** compute the factor from
  `base_width`/`base_height` at load; if a frame arrives at other dimensions,
  recompute the largest integer factor that fits and accept the size change. Log
  it. This is mostly SNES - base 256×224 gives a clean 3×, but hires 512×448 only
  fits at 1×, and games toggle hires for menus. Whether that looks acceptable is
  a judgment to make on hardware, not in a document.

## Consequences

**Easier:** Ports are testable without a core, so the desktop backend stays
viable - which is what keeps iteration fast. Scaling arithmetic lives in one
place. Rotation is structurally contained. The host can skip or duplicate a frame
without the core's knowledge, because it owns the buffer.

**Harder:** One memcpy per frame, measured above at ~1% of bandwidth.

**Deliberately deferred:** `RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER`
lets a core render straight into a host-provided buffer, eliminating that copy.
It is the *same* architecture - buffer the host owns, present after run - so it
slots in later without reshaping the seam. Not built now because the copy is not
measurably a problem, and an optimization with no measured need is speculative.

**Not settled here:** the pacing strategy itself (§6, §7) - audio-driven with DRC
is implied by `audio_queued` and non-blocking writes, but the loop is not
specified. Nor is `present_blocks`' effect on that loop.

## Revisit if

- Measured frame copy cost exceeds a few percent of frame budget on the Brick -
  then adopt `GET_CURRENT_SOFTWARE_FRAMEBUFFER`; or
- a port cannot be implemented without including `libretro.h`, which would mean
  the boundary is drawn in the wrong place; or
- a target device appears whose display cannot present a landscape surface.
