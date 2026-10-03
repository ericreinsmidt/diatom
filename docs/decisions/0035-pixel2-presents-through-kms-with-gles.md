# 0035. The GKD Pixel 2 presents through KMS with OpenGL ES, on a presenting thread

- **Status:** Accepted
- **Date:** 2026-10-01 (accepted the same day)
- **Supersedes:** -
- **Superseded by:** -

> The system this calls TortOS-px2 was renamed plastron on 2026-10-03
> ([github.com/ericreinsmidt/plastron](https://github.com/ericreinsmidt/plastron)).
> Nothing in the decision changed.

The second device port. [ADR-0013](0013-brick-fbdev-flip-thread.md) chose the
Brick's display path from that device's measurements; this does the same for
the Pixel 2, and keeps 0013's contract: `present()` never blocks.

## Context

The GKD Pixel 2 is an RK3326S (four Cortex-A35 at 1.3 GHz) with a Mali-G31,
1 GB of RAM, and a 640x480 panel that is physically **480x640 portrait**,
mounted turned. Its system is TortOS-px2, a Buildroot image of our own:
mainline Linux 7.1, Mesa's Panfrost (OpenGL ES 3.1 through GBM and EGL), SDL2
2.32 with KMSDRM and ALSA, no udev.

Measured on the device, 2026-10-01:

1. **Display.** DRM/KMS only: no fbdev, and the display controller has **no
   rotation** on its planes. Whoever draws has to draw turned. The kernel
   reports the panel as "Left Side Up"; a landscape picture turned 90 degrees
   counter-clockwise lands the right way up (checked by eye, a marker drawn
   top-left of the landscape image appeared top-left).

2. **The cores are about half as fast as on the Brick.** `tools/ffprobe`,
   3,600 unpaced frames from a cold start (title screens and attract mode, not
   the gameplay states of ADR-0034's table, so not strictly comparable):

   | core | game | p50 | p95 | worst |
   |---|---|---|---|---|
   | snes9x2010 | Donkey Kong Country | 9.43 ms | 10.51 ms | 11.28 ms |
   | mednafen_ngp | Metal Slug 1st Mission | 7.55 | 8.15 | 8.92 |
   | genesis_plus_gx | Gunstar Heroes | 5.26 | 6.09 | 8.03 |
   | mgba | Golden Sun | 5.26 | 7.06 | 13.81 |
   | fceumm | Contra | 4.23 | 4.40 | 5.34 |
   | mednafen_pce_fast | R-Type | 2.86 | 2.97 | 3.60 |

3. **Scaling on the CPU does not fit beside SNES.** A 256x224 RGB565 frame
   scaled nearest to 640x480 and turned onto the portrait buffer, 300 frames,
   by a throwaway probe outside the repository:

   | | per frame |
   |---|---|
   | CPU, walking the panel's rows (sequential writes), into scanout memory | 7.68 ms |
   | CPU, walking the game's rows (writes down columns), into scanout memory | 12.47 ms |
   | GPU through SDL's GLES renderer: CPU time to upload and draw | 2.69 ms |

   SNES at a 10.5 ms p95 plus 7.7 ms of scaling is past the 16.7 ms a refresh
   allows, before sharp-bilinear, the overlay or fast forward. The GPU figure
   includes SDL's own overhead; a direct path should cost less, unmeasured.

4. **Swaps block.** With vsync, `SDL_RenderPresent` returned at the panel's
   rate (58.9 fps over the run including start-up), the same shape that made
   0013 move the Brick's pan onto a thread.

## Options considered

### Option A - CPU scaling into DRM dumb buffers, with a flip thread
The closest copy of 0013: the existing blit, rotated, into dumb buffers, and
the thread page-flips. Smallest change and no GL. Rejected on measurement 3:
7.7 ms of the frame loop's 16.7 for the cheapest variant, nearest only, before
sharp or the overlay.

### Option B - CPU scaling on a second core
Run the blit on the flip thread instead of the frame loop's, using a spare
core. Keeps the code and gets the frame loop its time back. But it moves the
same 7.7 ms (more for sharp) onto a core the audio thread and the launcher
also want, adds a source-frame copy to hand over, and leaves fast forward
paying for every frame drawn. It is a fix for the CPU route's cost, not a
reason to keep that route when the GPU does the job for a third of the CPU.

### Option C - OpenGL ES on a presenting thread (chosen)
`present()` copies the core's frame into a latest-wins mailbox and returns.
A thread that owns the EGL context uploads it as a texture, draws it scaled
and turned in one quad, swaps, and page-flips. Nearest is the sampler's
nearest; sharp-bilinear becomes a fragment shader; the level bar and the
overlay are quads blended over the frame instead of pixels painted into pages
and cleaned off again.

### Option D - SDL's renderer
What the probe measured, and what the desktop port does. But it is a second
SDL video client on a display the launcher's SDL also drives, it hides the
DRM file the handover needs ([ADR-0036](0036-kms-handover-by-drm-master.md)),
and it adds SDL's overhead to every frame for convenience the port does not
need.

## Decision

Option C. `port/pixel2.c` opens the DRM device itself, creates a GBM surface
and an OpenGL ES 2 context on it, and presents from one thread:

- `present()` copies the frame (at most 512x480x4 bytes; 115 KB for SNES) into
  the mailbox and signals. Latest wins, as on the Brick. `present_blocks` is
  false.
- The thread uploads, draws, swaps and page-flips, waiting for the flip event
  before taking the next frame. RGB565 uploads as is; XRGB8888 by swizzle.
- The surface is reported as 640x480. The turn is in the quad's vertices, and
  nothing above the port learns of it: the rotation design of
  [ADR-0007](0007-port-interface.md) gets its first real implementation.
- Pixel format is ARGB8888, the format the launcher's SDL scans out, because a
  page flip cannot change format (ADR-0036).

## Consequences

- The frame loop spends a memcpy on presenting instead of a blit. Measured
  numbers for the real path come with the implementation.
- Sharp-bilinear has to be re-proven as a shader: interior pixels exact, and
  identical to nearest at an integer factor, the two properties ADR-0007's
  filter definition promises. Until a capture comparison shows that, the
  shader is unverified.
- `diatom_port_capture` reads back from the GPU and turns the image back to
  landscape. A capture is rare, so its cost does not matter; its correctness
  (the displayed frame, not the next one) is kept by draining the thread
  first, as on the Brick.
- The port links EGL, GLESv2, GBM and libdrm. That is the Pixel 2 system's own
  Mesa; nothing comes from another project's tree.
- Two ports now present in completely different ways. The seam is unchanged,
  which is the test 0007 set for it.

## Revisit if

- A direct GLES present measures above 4 ms of frame-loop or thread CPU per
  frame in `stretch`, which would erase most of the margin over Option B.
- Panfrost's swap starts quantizing the frame rate the way the Brick's `mali`
  EGL did (0013): measured as a sustained rate below the panel's 60.08 Hz with
  the thread otherwise idle.
