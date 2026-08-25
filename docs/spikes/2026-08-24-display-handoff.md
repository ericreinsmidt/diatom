# Spike result - display handoff, fbdev to EGL

- **Date:** 2026-08-24
- **Question:** *Can an fbdev presenter release the display to an EGL presenter
  and take it back, repeatedly, without wedging the display engine?*
- **Status:** Answered. **Yes**, with one invariant.
- **Instruments:** [`tools/eglpresent.c`](../../tools/eglpresent.c),
  [`tools/holdfb.c`](../../tools/holdfb.c), plus the real `diatom` binary as
  the fbdev side.

## Why it was in doubt

[ADR-0013](../decisions/0013-brick-fbdev-flip-thread.md) put the Brick port on
raw fbdev because the mali EGL swap cost two vblanks. PlayOS draws through EGL.
Earlier the same day, those two presenting **at the same time** stalled a
PowerVR fence and wedged `FBIOPAN_DISPLAY` in-kernel, unkillable, recoverable
only by power cycle.

[ADR-0016](../decisions/0016-saves-and-save-states.md) then put the in-game menu
in the launcher, reached by handing the display over on MENU. That makes this
pairing load-bearing rather than incidental. PlayOS and minarch hand off fine
today, but both are EGL, so their success was no evidence for our case.

## Method

Two parts, because the first does not test what the architecture actually does.

**Part one - handoff with full release.** Four cycles of: real `diatom` running
Final Fantasy for 180 frames, exit, EGL presenter for 90 frames, exit. Then a
fifth `diatom` run to check nothing degraded.

**Part two - handoff while the fbdev side stays alive.** ADR-0008 makes Diatom
long-lived, so a paused Diatom still holds `/dev/fb0` open and mmapped. A probe
pans 60 times, then **holds the fd and the mapping without panning** for six
seconds while the EGL presenter runs, then resumes panning.

`dmesg` was diffed across both.

## Result

**Part one, every cycle identical:**

```
fbdev : 180 frames in 3.00s = 60.09 fps, 0 resync(s), present avg 8.41 ms
egl   : ok, driver=mali 1024x768, 90 frames in 1475 ms
```

Five fbdev runs before, between and after four EGL runs. Frame rate, resync
count and present cost did not move by more than 0.02 ms. No new kernel
messages.

**Part two:**

```
HOLD: panned 60, 0 fails. holding fb0 open+mapped for 6s
EGL: ok, driver=mali 1024x768, 90 frames in 1476 ms
HOLD: resumed panning after EGL, 0 fails
```

No new kernel messages.

## What this establishes

**The invariant is "one presenter at a time", not "one process may hold the
display".** Holding `/dev/fb0` open and mapped while another process presents
through EGL is fine. What is fatal is both actually presenting.

That is the good version of the answer. A paused Diatom does not need to
release anything: it stops its flip thread and the launcher draws. No munmap,
no close, no re-acquire, no window where neither owns the screen.

**ADR-0016's menu design is viable as written.** Blind hotkeys and a
Diatom-drawn overlay were the fallbacks if this had failed; neither is needed.

## Incidental finding, worth not misreading

The EGL presenter ran 90 frames in 1475 ms, **16.4 ms per frame** - one vblank,
not the two that ADR-0013 measured. The difference is workload: this probe only
clears and presents, while Diatom uploads a game frame first. So the mali swap
is not unconditionally 30 ms; it waits for the next vblank, and real work makes
it miss the deadline and wait for the one after.

ADR-0013's decision is unaffected - with a real frame to upload it *does* cost
two vblanks, which is what was measured and why fbdev was chosen. But the
mechanism is "misses the deadline under load", not "always blocks twice", and
the difference would matter to anyone revisiting it.

## Not tested

Concurrent presentation was **not** re-attempted. It is already known to wedge
the device, it costs a power cycle, and nothing about the design needs it to be
true twice.
