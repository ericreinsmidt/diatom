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

## Open at end of session

- [ ] Re-measure pacing with Diatom as sole display owner (blocked on a
      power cycle at time of writing).
- [ ] Physical button verification: press each cap under
      `DIATOM_INPUT_DEBUG=1`, correct the A/B/X/Y assumption if wrong.
- [ ] Visual confirmation on the panel: the capture proves the blit, not the
      scanout; eyes on the screen prove scanout.
- [ ] `[LATER]` volume keys during play: the joystick device emits
      VOLUMEUP/DOWN codes; whose job is volume - port, host application, or
      firmware daemon - is undecided and deferred.
- [ ] Audio queue accounting: `audio_write` admits a batch when *any* space
      remains, so `queued` can overshoot capacity by one batch (measured max
      4816 of 4096). Harmless slop or fix; decide when touching audio next.
