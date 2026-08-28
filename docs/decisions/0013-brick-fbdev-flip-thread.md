# 0013. The Brick presents via fbdev, with a flip thread

- **Status:** Accepted
- **Date:** 2026-08-24
- **Supersedes:** -
- **Superseded by:** -

## Context

ADR-0012 assembled the Brick sysroot around the firmware's own SDL2, and the
first working port used it for everything: video, audio, input, clock. The
video half failed on measurement, in layers.

**All measured on the device, firmware 1.1.1, 2026-08-24:**

1. **The firmware SDL2 contains exactly two video drivers: `mali` and
   `dummy`.** No kmsdrm, no fbdev, no x11. The mali driver is the only real
   display path SDL offers.
2. **The mali EGL swap blocks ~30ms** - two vblank intervals - regardless of
   swap interval. `SDL_RenderSetVSync(0)` returns success and changes nothing.
   Frame loop quantised to 30.1fps (GPU renderer) and 20.1fps (software
   renderer): integer divisors of the panel rate, the signature of a blocking
   swap.
3. **Scanout is not DRM.** `/dev/dri/card0` is the PowerVR render node
   (`pvrsrvkm`), display-incapable. The panel is driven by the Allwinner disp2
   engine, `/dev/fb0`, running at 60.9Hz measured (nominal 60).
4. **`FBIOPAN_DISPLAY` is itself a blocking, vsync-latched flip.** A pan
   issued just after vblank returns in one interval - a tight pan loop
   sustains 60fps - but one issued mid-interval misses the latch deadline and
   waits for the vsync after next (~25ms). Activate flags do not change this;
   `FBIO_WAITFORVSYNC` is a 1μs no-op. A self-paced frame loop always lands
   mid-interval, so panning inline halved the frame rate exactly as the EGL
   swap had: 29.4fps.
5. **The CPU blit is cheap.** 256x224 RGB565 scaled 3x into the 32-bit
   framebuffer: 4.7-4.9ms, steady.

The port seam requires `diatom_port_present()` to never block (ADR-0007), and
the frame loop paces against its own absolute clock precisely because no
console runs at the panel's rate. A present that blocks to the panel is not an
inconvenience; it contradicts both decisions at once.

## Options considered

### Option A - SDL renderer over the mali driver
Measured: 30fps ceiling. Rejected on that number alone.

### Option B - pace the loop off the blocking flip (vsync pacing)
Lock the loop to the panel and let the flip be the clock. Runs NTSC content
0.5-2% fast, and PAL content - a deliberate target, measured 50.007fps - 20%
fast. Rejected: it repeals ADR-0011's premise that content rate and panel rate
are unrelated.

### Option C - pan inline from the frame loop
Measured: 29.4fps, because pan blocks to the latch point. Rejected.

### Option D - fbdev with a dedicated flip thread
`present()` blits into a free page and publishes it to a latest-wins mailbox;
a flip thread pans at the panel's own cadence and absorbs the blocking wait.
Three pages: on glass, in flight, being drawn. Flips latch at vblank, so no
tearing; nothing in the frame loop ever waits on the display.

## Decision

**Option D.** Presentation is raw fbdev plus a flip thread. SDL2 remains for
what it does well here: audio, joystick input, the monotonic clock.

Measured result: 58.1fps against a 59.7275 target with zero resyncs and a
non-blocking 4.7ms present - while the stock UI stack was still running and
competing for the display. The remaining deficit is being re-measured with
single ownership.

## The display-ownership consequence

The register (§3) carried an open risk: *display handoff between two processes
will need re-solving per port and is the likeliest source of platform-specific
pain.* It now has a measured instance. Running Diatom's pan loop while another
process presented through EGL wedged the pan **inside the kernel**
(`lock_fb_info`, unkillable): the other client's stalled GPU work held a
PowerVR SyncFb fence, and the framebuffer lock queued behind it forever.

So on this platform the rule is not "be careful", it is **exactly one display
client, enforced by the host application**. ADR-0009's `EXIT`-before-relaunch
rule is that enforcement at the protocol level; a launcher that starts Diatom
must itself stop presenting first.

## Consequences

**Easier:** The frame loop is identical to desktop - same clock, same rate
control, same numbers to compare. The port honors `present_blocks = false`
truthfully. No dependency on the firmware's video stack beyond the kernel's
own fbdev ABI.

**Harder:** The port owns a thread and a hand-written scale blit. The blit
spends 4.7ms of CPU per frame that a working GPU path would not; acceptable
now, and the number is in the present-debug log the day it is not.

**Risk carried:** single-ownership is enforced by convention until the PlayOS
side of ADR-0009 exists. During development that means stopping the device UI
before running Diatom by hand.

## Postscript, same day

The single-owner re-measurement closed the deficit entirely: **59.73fps
against 59.7275, 0 resyncs, drift +0.06%**. The 58.1 figure was a measurement
bug - the stats clock was read after the screenshot write - not a pacing
deficit. One more scanout fact joined the pile: the disp2 engine composites
the fb layer with per-pixel alpha, so the blit must write the alpha channel
opaque or the panel shows black while capture reads back perfect frames. See
the session log.

## Revisit if

- a firmware update gives SDL a non-blocking present or a real KMS path; or
- a core plus the 4.7ms blit no longer fit the frame budget (the disp2 engine
  can scale layers itself; a hardware path exists below fbdev); or
- single-ownership proves unenforceable, in which case presentation moves to
  `/dev/disp` layer ioctls where clients can coexist.
