# 2026-08-24 - the Brick port, from toolchain to first frames

Executing ADR-0012 and building `port/brick.c`. Everything below ran on the
device (firmware 1.1.1) over USB ADB, same day.

## Toolchain and sysroot

- `tools/brick-toolchain.Dockerfile`: `debian:bullseye-slim` pinned by digest,
  stock `aarch64-linux-gnu` GCC 10.2.1. Built and verified; runs natively on
  an arm64 host, no emulation.
- `tools/fetch-brick-sysroot.sh`: pulls the device's own `libSDL2-2.0.so.0`
  over ADB, resolves its dependency closure with the container's readelf
  (result: glibc family only - the GPU and audio stacks are dlopen'd), and
  fetches SDL2 2.30.8 headers from the upstream release tarball, sha256
  pinned. Writes a `PROVENANCE` file into the sysroot.
- The release tarball's `SDL_config.h` falls back to `SDL_config_minimal.h`
  on Linux. Checked: it defines `HAVE_STDINT_H` for GCC, so sized types are
  right, and no public ABI depends on the rest. Sound for an API consumer.
- First cross build linked clean against the device library on the first try,
  demanding at most `GLIBC_2.17` against the device's 2.33.
- Makefile: objects moved to `build/$(PORT)/` so host and cross builds cannot
  collide; `tools/brick-make.sh` runs make inside the container.

## The port, in three acts

**Act one - SDL video, 30fps.** The mali EGL swap blocks two vblank intervals
no matter what. Renderer timing splits present() as upload 2ms / draw 50μs /
swap 30ms. Full story and numbers in
[ADR-0013](../decisions/0013-brick-fbdev-flip-thread.md).

**Act two - fbdev inline, 29fps.** Scanout turned out to be disp2 fbdev, not
DRM (`card0` is the PowerVR render node). Panning fb0 inline: blit 4.8ms, pan
27.8ms. A pan micro-benchmark (throwaway spike, `pantest.c`) showed pan is a
vsync-latched blocking flip: a tight loop sustains 60fps, a mid-interval call
slips to the vsync after next. A self-paced loop always calls mid-interval.

**Act three - flip thread, 58fps and honest.** present() blits and publishes
to a latest-wins mailbox; a thread eats the blocking pan. Zero resyncs,
present cost 4.7ms non-blocking, audio holding around target instead of
draining. The measurement ran with the stock UI still fighting for the
display; the single-owner number is being taken next.

## The register earned its keep

§3 carried "display handoff between two processes... likeliest source of
platform-specific pain" as an open risk since scoping. Today it happened on
schedule: PlayOS's UI (this device runs PlayOS, and its launcher also owns
adbd) was presenting through EGL while Diatom panned fb0. PlayOS's GPU
context stalled holding a PowerVR SyncFb fence, and Diatom's pan wedged
in-kernel (`lock_fb_info`), unkillable. Recovery needed a power cycle - and
killing the launch chain to free the display also killed adbd, which is a
lesson about this device, not about Diatom: **stop `playos.elf` and
`minarch.elf` only, never `launch.sh`.**

## Input, derived rather than guessed

The Brick's controls are one kernel joystick, `TRIMUI Player1` (the two
keyboard devices carry only volume and power). Decoding its capability
bitmask from `/proc/bus/input/devices` gives the exact button set; SDL
assigns indices in ascending code order, so the index side of the map is a
derivation. Dpad is `ABS_HAT0`; L2/R2 are axes (no `BTN_TL2/TR2` in the
mask). What stays provisional is which physical cap emits which code -
`DIATOM_INPUT_DEBUG=1` logs raw events to settle that in one session with
hands on the device.

## The single-owner measurement, and a measurement bug

Getting exclusive display ownership took three attempts, each teaching one
device fact:

1. Sweeping every PlayOS process killed adbd - it lives under the launch
   chain.
2. `killall playos.elf minarch.elf` alone does not stick: **`launch.sh` is a
   supervisor loop and respawns them**, which quietly re-created the
   two-client display fight mid-test and wedged the GPU firmware so hard that
   PVR could not free the dead process's context ("Retry limit reached");
   only a reboot clears that state.
3. The sequence that works: `kill -STOP` the supervisor, then kill the UI
   processes while healthy. Zombies remain (frozen parent cannot reap);
   harmless.

The lingering 2.8% fps deficit then turned out not to exist. The stats clock
was read *after* `diatom_port_capture()`, and writing a full-screen BMP costs
~430ms; the loop was pacing perfectly while the report said otherwise. A
measurement bug wearing the costume of a pacing bug - fixed by reading the
clock before the capture.

**Final, sole owner, 900 frames: 59.73fps against 59.7275, 0 resyncs, rate
control drift +0.06%, audio holding around target, clean exit.** The capture
shows the stub pattern pixel-exact: border intact, hires stripes resolved,
letterbox at the locked rect.

## Capture is not scanout: the alpha lesson

With pacing perfect and the capture pixel-exact, the panel showed black.
The disp2 engine composites the fb layer in per-pixel alpha mode
(`a[pixel 255]` in the engine dump), the framebuffer format is ARGB
(transp at 24+8), and the blit was writing zero alpha: every pixel fully
transparent, composited over black. Capture reads memory; the panel reads
alpha. The port now fills the format's transparency channel opaque, read
from the driver like the colour offsets, and the pattern showed on glass.

The general lesson joins the register's themes: each stage of today's
pipeline validated the previous one and something orthogonal still failed -
link proved load, load proved run, run proved blit, blit proved memory, and
memory is still not light. The only proof of the last step was eyes on the
panel.

## Input map verified by hand

Two ordered pass-throughs of every control under `DIATOM_INPUT_DEBUG=1`.
The derived index table held exactly; the positional label assumption was
wrong in one place: **X and Y are swapped** relative to position - the top
cap (X) emits BTN_WEST, the left cap (Y) emits BTN_NORTH. A=1, B=0
confirmed; L1=4, R1=5; L2/R2 are axes 2/5 resting at -32768 and slamming to
+32767; Select=6, Start=7, Menu=8 (confirmed by exiting the session). The
map in port/brick.c is now measured, not argued.

## Where the port stands

**59.73fps against a 59.7275 target, 0 resyncs, rate control drift well
inside bounds, clean exit, pattern verified on the physical panel, every
button verified by press.** The Brick port is real.

## First real game

Contra (USA) on FCEUmm (the exact binary the environment inventory
measured), same evening. FCEUmm reports NTSC 60.0998fps and native 48000 Hz,
so the resampler ran at near-unity - a good test of rate control around 1.0.
Display rect locked at 768x720, the largest integer scale of 256x240 that
fits the panel (ADR-0007 policy; the letterbox prompted the question and the
answer is intentional).

Scripted run: 900 frames at 60.10fps, 0 resyncs, drift +0.04%.

Then a human played it for three minutes - jungle stage cleared, into
Base 1: **10,664 frames in 177.44s = 60.10fps against 60.0998, 0 resyncs,
drift -0.21%, audio holding around target throughout.** Player's verdict:
felt smooth.

One number to keep an eye on: the blit costs 9.3ms under a real core
against 4.7ms under the stub - same destination size, colder caches. It
fits the NTSC budget with room, but SNES hires plus a heavier core will
squeeze; ADR-0013 already names the disp2 hardware scaler as the escape
hatch if CPU blitting ever stops fitting.

## Display modes, built to be looked at rather than argued about

The letterbox around Contra prompted the obvious question, and the honest
answer was that integer-fit is a policy with five defensible alternatives and
no measurement behind the choice. So all six exist now - native, integer,
integer-overscale, aspect-fit, aspect-fill, stretch - crossed with two filters,
nearest and sharp-bilinear. Mode and filter cycle independently on device
(SELECT+shoulders, SELECT+A) because the comparison that matters is one
geometry with and without blending, which a flat preset list puts two presses
apart. **Nothing is decided; `integer` stays default only because it already
was.**

Two assumptions died on contact with the measurement:

**FCEUmm reports an 8:7 pixel aspect, 1.2190, not 4:3.** The prediction that
fit, fill and stretch would collapse into one rect on a 4:3 panel was wrong:
they are 936x768, 1024x840 cropped, and 1024x768 - three different pictures.
The preset model was rebuilt as two independent axes because of it.

**Sharp-bilinear at a whole factor was costing 2.4ms to compute pixels
identical to nearest.** True by construction, so the map builder now collapses
any axis whose weights are all 0 or 256 onto the fast path; integer+sharp went
from 8.34ms to 5.93ms, matching nearest exactly.

Measured on device, Contra on FCEUmm, 240 frames each, **all twelve
combinations at 60.10fps with zero resyncs**:

| mode | rect | nearest | sharp |
|---|---|---|---|
| native | 256x240 | 0.69 ms | 0.68 ms |
| integer | 768x720 | 5.96 ms | 5.93 ms |
| aspect | 936x768 | 7.70 ms | 14.98 ms |
| fill | 1024x840 crop | 8.45 ms | 14.21 ms |
| stretch | 1024x768 | 8.42 ms | 14.49 ms |
| overscale | 1024x960 crop | 8.42 ms | 13.84 ms |

The number that matters is the sharp column against a 16.64ms budget: it fits
NTSC on the lightest core in the test matrix with under 2ms to spare. That is
not headroom, it is luck, and a heavier core will take it. Filed as an open
question rather than a working feature.

Nearest at a fractional factor is visibly uneven - at 3.656x horizontally,
letter strokes in Contra's menu come out three or four pixels wide at random.
Sharp fixes it for about 6ms. Whether that trade is worth making is the thing
to look at on the panel.

## Taking the display is now a script, not tribal knowledge

`tools/brick-run.sh` plus the on-device `tools/brick-device-run.sh`: freeze the
supervisor, kill the UI, run, restore in a trap. Both traps from earlier today
are encoded in it, which is the point - the operational knowledge that cost a
power cycle to learn should not live only in a session log. Everything is
staged at `/mnt/SDCARD/diatom/`, on the card rather than `/tmp`, because `/tmp`
is tmpfs and a reboot empties it.

## The filter finding that inverted the cost model

`aspect` is the cheapest geometry with nearest (7.73 ms) and by far the most
expensive with sharp (14.98 ms). It is the only mode where both axes are
fractional: 936/256 = 3.656 across, 768/240 = 3.2 down. Every fullscreen mode is
exactly 4.0 horizontally, so its horizontal map collapses onto the fast path and
only the vertical blends, costing about 2 ms rather than 7.

So which mode is affordable depends on which filter is on, and it inverts. Worth
remembering before assuming a "cheap" mode stays cheap.

## Open at end of session

- [x] **Default display mode decided** by looking at the panel across two
      sessions, ~20 minutes of play, 27825 frames in the second alone:
      **`stretch` + `nearest`**, all six modes shipped and selectable
      ([ADR-0014](../decisions/0014-display-modes-and-default.md)). A handheld's
      screen is its whole interface and full use of it beat both the letterbox
      and the crop. Sharp earned nothing visible at these factors and is the only
      thing that has ever made the loop miss a frame - 2 resyncs, both in
      `aspect sharp`, the only combination over 11 ms.
- [ ] `[LATER]` volume keys during play: the joystick device emits
      VOLUMEUP/DOWN codes; whose job is volume - port, host application, or
      firmware daemon - is undecided and deferred.
- [ ] Audio queue accounting: `audio_write` admits a batch when *any* space
      remains, so `queued` can overshoot capacity by one batch (measured max
      4816 of 4096). Harmless slop or fix; decide when touching audio next.
- [ ] Next milestone: a real core and a real game on the Brick - cores are
      already on the device from the spike runs.
- [ ] Restoring the device UI after tests: `kill -CONT` the frozen
      `launch.sh` (its loop respawns the UI) or reboot.
