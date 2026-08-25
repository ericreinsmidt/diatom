# Crash reporting: `EXIT reason=crash`

ADR-0009 specified three exit reasons and Diatom emitted one. `EXIT reason=user`
was sent whether the player stopped the game or the core died on its face, so a
launcher had no way to say "Contra crashed" rather than "Contra ended".

## Why this cannot be reported the way SIGTERM is

`on_terminate` does almost nothing: it sets a flag, and the frame loop notices
after `retro_run` returns and does the real work there. That is the right shape
for SIGTERM, which on the Brick arrives about 810 ms before the process dies.

It is not available for a crash. A core is `dlopen`'d into this address space
(ADR-0006), so a core that segfaults segfaults Diatom, and there is no "after
`retro_run` returns" - the process is going away inside the handler. So the
handler has to send the message itself, under async-signal-safety rules:

- **No formatting.** `snprintf` is not on POSIX's safe list, and neither is
  re-entering the buffer `diatom_proto_send` formats into. The lines are
  therefore complete constants, one per signal per phase. Repetitive on purpose:
  assembling a line from pieces risks a torn write putting a malformed message
  on the socket at the worst possible moment.
- **`send`, not `write`.** `send` is on the safe list, and `MSG_NOSIGNAL` stops
  a launcher that died first from turning a reportable crash into an
  unreportable one - the SIGPIPE default disposition would kill us before we
  could re-raise the real signal.
- **One pass, no EAGAIN retry.** A launcher that stopped reading would otherwise
  turn a crash into a hang. An unreported crash is much the lesser fault: the
  socket closing tells the launcher something died regardless.

## The phase decides the message, and the rule already existed

> ERROR means the game never started. EXIT means it ran and stopped.

That rule is about the **display**, so it answers this question completely. A
core that dies while loading means the launcher is still drawing and must hear
`ERROR`. A core that dies mid-game means the launcher is *not* drawing and must
hear `EXIT`, or a handheld sits on a dead frame. One `sig_atomic_t` phase
covers it: idle, loading, running.

Idle deliberately reports nothing. Diatom dying between games is not a fact
about any game, and the launcher learns it from the socket closing - which
ADR-0009 already relies on for liveness.

`run_session` was split so the phase is set and cleared in a wrapper rather than
on each of the six early returns inside. A seventh return added later cannot
forget to clear it.

## What does not raise a signal

A core that calls `exit()` on a fatal error - some do - produces no signal at
all, and the launcher would wait forever on a process that had already gone. An
`atexit` hook covers it, guarded on the phase so a clean exit adds nothing.

## Measured

Six deliberate failures, driven through the socket on the Brick, 2026-08-25.
The fixture is `test/stubcore.c` under `STUBCORE_CRASH`; the frontend has no
crash-triggering code in it, which is the point - Diatom learns about the crash
the same way it will in the field, from a signal it did not raise.

| Mode | Reported | Process died | Correct because |
|---|---|---|---|
| `segv` | `EXIT reason=crash signal=SIGSEGV` | signal 11 | post-RUNNING, so the launcher takes the display back |
| `stack` | `EXIT reason=crash signal=SIGSEGV` | signal 11 | stack exhaustion still reports - see below |
| `abort` | `EXIT reason=crash signal=SIGABRT` | signal 6 | `assert` and explicit `abort` land here |
| `exit` | `EXIT reason=crash msg=core called exit` | rc=1 | no signal at all; the `atexit` path |
| `load` | `ERROR code=crash msg=SIGSEGV` | signal 11 | pre-RUNNING, so the launcher keeps the display |
| `fpe` | nothing | - | ARM does not trap integer divide by zero |

SIGBUS and SIGILL are in the table but were **not exercised** - they take the
same path as SIGSEGV and the table is data, not code, but that is an argument
rather than a measurement and should not be read as one.

Every crash still dies of the original signal with its original disposition.
Anything else lies about how the process ended: the wait status is how a
supervisor tells a crash from a clean stop, and the core dump is how anyone
finds out why. Returning from a SIGSEGV handler instead would re-run the
faulting instruction forever.

A normal session still reports `EXIT reason=user`, and that is not a leftover
placeholder. Every way of reaching it is someone deciding to stop: STOP or QUIT
from the launcher, MENU when standalone, a closed window, or SIGTERM - which on
the Brick is the power button, so still the player. ADR-0009's third reason,
`error`, has no producer: there is no post-RUNNING failure Diatom can currently
detect that is not a crash. Left unproduced rather than invented.

## The alternate signal stack is load-bearing, not defensive

The failure most likely to arrive is a core recursing without bound, and a
SIGSEGV raised by an exhausted stack cannot run a handler on that same stack.
A/B on the Brick, one binary, changing only `SA_ONSTACK`:

| Build | Result |
|---|---|
| `SA_ONSTACK` | `EXIT reason=crash signal=SIGSEGV` |
| without it | nothing; the launcher saw only the socket close |

Both died on signal 11. What the flag buys is the **report**, not the death.

It is per-thread, so this covers the main thread only. The port's flip thread
does not recurse; a fault there still reports, just not the overflow case.

## Three things this found that were not the feature

**The fixture was not testing what it claimed.** `stack` crashed on macOS and
did not on the Brick, which read as a platform difference. It was a compiler
difference: `aarch64-linux-gnu-gcc -O2` turned the recursion into
`add sp, sp, #0x210; b recurse` - a sibling call that pops its frame before
branching, so the stack never grew. `volatile` and `noinline` do not prevent
that; only using the frame *after* the call does. Confirmed by disassembly
before and after, and this is the second time a test has passed on nothing
(the first was the blit comparison on 2026-08-24). **A fixture that agrees with
you is not evidence until you have seen it disagree.**

**The instruments were discarding their own output.** Both Diatom and protodrive
block-buffer stdout when redirected, so a process killed while waiting lost
everything since the last 4 KB boundary. That made a session that reached
RUNNING look like one that never had. Both are line-buffered now. For Diatom
this is not only a test concern: after a crash the log is where you look for
why, and the crash handler cannot flush it - flushing is not signal-safe. Tens
of lines per session, not per frame, so the cost is noise.

**`QUIT` did nothing to an idle Diatom.** It fell through to `continue` in the
idle poll, so only a *running* Diatom could be shut down - the opposite of what
a launcher needs, since between games is exactly when it would ask. Every crash
run had been ending on the watchdog's SIGKILL rather than on QUIT, and the
watchdog made that look normal. Two lines.

The staleness guard in `tools/brick-make.sh` also did not cover the stub core,
so a fixture that had been fixed was still the broken one on the device. Now it
does.

## Still open in the protocol

- `PREVIEW` is not emitted.
- Display mode is not carried per RUN; it stays at whatever the process started
  with.
- The audio queue admits a batch when any space remains, overshooting capacity
  by one batch - measured 4927 of 4096.
