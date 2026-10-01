# 0036. On KMS, the display is handed over by DRM master, at present and present_stop

- **Status:** Accepted
- **Date:** 2026-10-01 (accepted the same day)
- **Supersedes:** -
- **Superseded by:** -

[ADR-0008](0008-separate-long-lived-process.md) made Diatom a long-lived
process beside the launcher and said the display handover is re-solved per
port. On the Brick it is "one presenter at a time" over a shared fbdev
(ADR-0013, `diatom_port_present_stop`). This is the answer for a KMS device,
first the GKD Pixel 2 ([ADR-0035](0035-pixel2-presents-through-kms-with-gles.md)).

## Context

Under KMS only the **DRM master** may set a mode or flip a page. The launcher
(TortOS, SDL2's KMSDRM driver) holds master for its window's whole life, and
both processes stay alive across every game - which is the speed the whole
design exists for, and not negotiable here.

Measured on the Pixel 2, 2026-10-01, with two throwaway stand-ins outside the
repository - an SDL2 KMSDRM + GLES program for the launcher, and a dumb-buffer
page-flipper for Diatom - passing master back and forth:

1. **It works.** 100 handovers in a row, watched on the panel: no black frame,
   no garbage, no tearing.
2. **Timing.** Launcher to Diatom 32.6 ms on average (worst 33.1), two
   refreshes; back 16.9 ms (worst 17.6), one. The extra refresh one way is the
   launcher's last frame still queued when it lets go; Diatom's first flip
   came back `EBUSY` every time and succeeded one refresh later.
3. **A page flip cannot change pixel format.** XRGB8888 buffers flipped onto a
   CRTC showing SDL's ARGB8888 were refused with `EINVAL`. Same format, fine.
4. **Opening the DRM device while nobody is master makes the opener master**,
   and a memory mapping of a buffer keeps that file - and the mastership -
   alive after `close()`. Found when the launcher stand-in could not start
   after a probe had closed its fd but not unmapped.
5. **Removing the framebuffer on glass blanks the panel.** The kernel disables
   the plane when a displayed framebuffer is removed (seen in the DRM debug
   log when a stand-in exited).

## Options considered

### Option A - pass DRM master at the moments the protocol already marks (chosen)
Diatom takes master on its first present after a stop and drops it at the end
of `present_stop`; the launcher drops it before `RUN` and `RESUME` and takes
it after `PAUSED` and `EXIT`. The protocol is unchanged: those lines already
mean "the other side has the display now". Measurement 1 says it is clean.

### Option B - one presenter: Diatom renders off-screen and the launcher presents
Diatom hands frames to the launcher (dmabuf over the socket) and never touches
the display. No mastership at all. But the launcher would have to run a
present loop during every game, which contradicts "the launcher stops drawing
on RUNNING", puts a second process in every frame's path, and is a large
change to both sides for a problem Option A solves in a few calls.

### Option C - tear the launcher's display down for each game
Closes the question by having only one client at a time. Measured cost on the
Brick of bringing SDL video back: about 600 ms. Rejected outright; it is the
thing TortOS exists not to do.

## Decision

Option A, with what the measurements taught written into the port:

- Diatom holds master only between its first `present` after a stop and the
  end of the next `present_stop`. It drops the mastership the kernel hands it
  at open (measurement 4) immediately, so a resident Diatom never holds the
  display while the launcher draws.
- Its buffers are ARGB8888 (measurement 3), and it never sets a mode: it flips
  onto the CRTC the launcher, or the boot splash, already lit. Standalone,
  with nothing lit, it sets the mode itself.
- A first flip refused with `EBUSY` waits one vblank and retries
  (measurement 2). Diatom's game load, 26 to 44 ms from `RUN` on the Brick,
  covers that wait.
- `present_stop` drains the thread, waits for the last flip, parks (KEEP
  leaves the last frame, BLANK flips to black), then drops master.
- Diatom's buffers live as long as the process (measurement 5): freeing the one
  on glass would blank the panel under the launcher.

## Consequences

- The launcher needs the matching change: drop master on the DRM fd SDL
  reports (`SDL_GetWindowWMInfo`, `info.kmsdrm.drm_fd`) before `RUN`/`RESUME`,
  take it after `PAUSED`/`EXIT`. Recorded as TortOS work.
- If Diatom dies holding master, the kernel releases it when the process goes,
  so the launcher can take it back. If it dies with its frame on glass, that
  framebuffer is removed and the plane goes dark until the launcher flips; the
  launcher's first frame after `EXIT` restores the picture. The launcher must
  treat a failed `drmSetMaster` as fatal to presenting and say so, not draw
  into a display it does not own.
- The stand-ins that produced these numbers lived outside the repository. If
  this has to be re-checked, they become an instrument in `tools/` under
  practice 7 rather than being rewritten from memory.

## Revisit if

- A handover on the real port shows a black or wrong frame, or launcher to
  Diatom takes more than three refreshes.
- SDL's KMSDRM driver starts holding state that does not survive another
  client flipping its CRTC: measured as the launcher's first flip after `EXIT`
  failing.
