# 0009. The launcher ↔ Diatom protocol

- **Status:** Accepted
- **Date:** 2026-08-23
- **Supersedes:** —
- **Superseded by:** —

## Context

[ADR-0008](0008-separate-long-lived-process.md) makes Diatom a separate process,
so a protocol is required. It is also the interface EROS, Contrarian and any
future firmware would be stuck with, which makes it worth more care than its size
suggests.

**What PlayOS does today**, read from `src/platform.c` and `src/main.c`:

```
/tmp/playos_req       fifo    →  "NES\t/path/core.so\t/path/rom.zip\n"
/tmp/playos_rep       fifo    ←  one byte, any byte, meaning "done"
/tmp/playos_res.pid   file       liveness, checked with kill(pid, 0)
/tmp/resume_slot.txt  file       the save slot, written just before the request
SIGUSR1               signal     ends the game (power button)
```

**Five mechanisms doing one job**, and PlayOS's own comments record why each
appeared. The fifos cannot report liveness — *"a resident that crashed leaves
both nodes behind and a check that just stats them says yes forever"* — so a pid
file was added. The request line could not carry the resume slot, so a temp file
was added. There was no way to say *stop*, so a signal was added.

That is accretion, not bad engineering: each piece patches a transport that could
not express what was needed. Three costs follow:

1. **The reply is one byte.** Clean exit, crash, missing BIOS, unreadable ROM and
   missing core are indistinguishable, so the launcher cannot show a useful error.
2. **Positional fields, no version.** Adding one means changing both sides in
   lockstep forever.
3. **No events during a game.** Between request and reply the launcher is blind.

Two further facts shape the design:

- PlayOS's launcher **already polls** rather than blocking — `plat_resident_wait()`
  is a 20 ms `usleep` loop that reads the fifo non-blocking, drains the power
  button, and checks `kill(res, 0)`.
- The launcher **keeps drawing its launch animation while the game loads**:
  *"the two do overlap for the length of this animation."* That overlap is a
  large part of why a launch feels instant, so the protocol must permit it.

## Options considered

### Transport

**Two fifos (status quo).** Familiar, filesystem-visible. But no connection
semantics, so peer death needs a separate pid mechanism, and both ends must be
held `O_RDWR` to dodge blocking-on-open and EOF races — a workaround PlayOS
already carries.

**`socketpair` + fork.** Simplest lifecycle, but ties Diatom's life to the
launcher. Incompatible with `launch.sh`'s restart loop, which depends on the
resident process surviving a launcher restart.

**Unix domain socket, `SOCK_STREAM`, Diatom listening.** Bidirectional over one
fd. **Peer death is native** — `read()` returns 0 on EOF, `connect()` fails when
Diatom is absent — which deletes the pid file outright. Supports `SCM_RIGHTS` if
display handoff ever needs fd passing. Connection-oriented but not
lifetime-coupled.

### Encoding

**Positional tab fields (status quo)** — no extensibility. **JSON** — needs a
parser for a handful of message types. **Tab-separated `key=value`** — parses
with `strtok`, readable, `socat`-debuggable, and named keys mean a new field does
not break an older peer.

## Decision

**One Unix domain socket.** `SOCK_STREAM`, Diatom listens at a path given by
argv or environment (defaulted), so it stays runnable on the desktop backend.

**Line-based, tab-separated `key=value`.** Tabs separate fields so paths may
contain spaces. Unknown verbs and unknown keys are ignored, for forward
compatibility.

```
RUN\tcore=/mnt/SDCARD/PlayOS/cores/fceumm_libretro.so\trom=/mnt/SDCARD/Roms/NES/Contra.zip\ttag=NES\tslot=9\n
```

**Launcher → Diatom**

| Message | Meaning |
|---|---|
| `RUN core= rom= tag= slot=` | load and run |
| `STOP` | end the current game |
| `QUIT` | shut down |

**Diatom → Launcher**

| Message | Meaning |
|---|---|
| `READY proto=1 state=idle\|running` | sent on connect |
| `RUNNING` | game is up; **Diatom owns the display** |
| `EXIT reason=user\|error\|crash` | the game ran and stopped |
| `ERROR code= msg=` | `bios_missing`, `rom_unreadable`, `core_missing`, … |
| `SAVED slot=` · `PREVIEW path=` | optional progress events |

### The rule that governs the state machine

> **`ERROR` means the game never started. `EXIT` means it ran and stopped.**

After `RUN`, exactly one of `{RUNNING, ERROR}`. After `RUNNING`, exactly one of
`{EXIT, ERROR}` — where a post-`RUNNING` failure is `EXIT reason=crash`.

This is about the **display**, not error reporting. `RUNNING` means "stop
drawing". If a missing BIOS were reported as `RUNNING` then immediately `EXIT`,
the launcher would relinquish the screen for a game that never existed — a black
frame before the shelf returns. The launcher must always know whether it still
owns the display.

`RUNNING` also turns PlayOS's launch-animation overlap from a timing coincidence
into a stated contract: `RUN` → the launcher may keep drawing; `RUNNING` →
Diatom owns the display; `EXIT` → the launcher takes it back.

### Supervision

The launcher keeps a loop while a game runs — **not to wait, but to supervise.**
Normally Diatom handles the power button itself while it owns input. But **a core
wedged inside `retro_run` cannot answer anything**, and the launcher's independent
power watch is then the only way out of a hung game. A supervisor that blocks
cannot rescue anything.

Shape it as `poll({socket_fd, power_input_fd}, -1)` rather than the current 20 ms
`usleep`: blocks efficiently, wakes instantly on either, no latency floor, no
spinning.

**One connection at a time.** A second concurrent connect is refused. Dead
launchers clean themselves up via EOF — including on `SIGKILL` — so the restart
case needs no policy about displacing a live connection.

**`state=` on connect exists for that restart case.** `launch.sh` can restart the
launcher while Diatom holds a running game; without it, the new launcher would
draw the shelf over live output.

## Consequences

**Easier:** Five mechanisms become one. Errors are legible for the first time, so
the launcher can say *"BIOS missing"* instead of returning silently to the shelf.
Adding a field no longer requires a lockstep change. Peer death is detected by the
transport rather than by a pid file that lies after a crash.

**What each side channel becomes:**

| Today | Becomes |
|---|---|
| `/tmp/resume_slot.txt` | `slot=` in `RUN` |
| `SIGUSR1` | `STOP` |
| `/tmp/playos_res.pid` | socket EOF and failed `connect()` |
| one-byte reply | `EXIT reason=` and `ERROR code=` |

**Harder:** More code than a fifo write. PlayOS must be changed on both sides,
and the protocol becomes a compatibility surface once another firmware adopts it —
which `proto=` exists to manage.

**Retained deliberately:** `SIGUSR1` stays documented as a **last-resort escape
hatch**, not the normal stop path. A core stuck inside `retro_run` will not read
the socket, and a signal will still interrupt it. This is the one place the
five-mechanisms-to-one story keeps a second channel, and it earns it.

## Revisit if

- Another firmware needs a message the verb set cannot express — bump `proto=`
  rather than overloading an existing verb; or
- the launcher needs to hand Diatom a file descriptor for display handoff, at
  which point `SCM_RIGHTS` is available and the transport already supports it; or
- text parsing shows up in a profile, which would be surprising at a few messages
  per game.
