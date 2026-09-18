# 0033. The newest connection wins, because a dead launcher's socket can outlive it

- **Status:** Accepted
- **Date:** 2026-09-18 (accepted 2026-09-18)
- **Supersedes:** [0009](0009-launcher-protocol.md), for its one-connection rule
  only. The rest of 0009 stands.
- **Superseded by:** -

## Context

ADR-0009 decided: *"One connection at a time. A second concurrent connect is
refused. Dead launchers clean themselves up via EOF - including on `SIGKILL` -
so the restart case needs no policy about displacing a live connection."*

On 2026-09-18 the Brick locked up at the shelf, and both halves of that turned
out to be false. Measured on the device, from `/proc`:

1. **A dead launcher's connection did not reach EOF.** TortOS starts Muse, its
   audio daemon, with fork and exec, and the daemon inherited every descriptor
   the launcher had open - its connection to this socket among them, with the
   display device, four input devices and five copies of a font. The launcher
   was then restarted. Muse kept the old connection's other end open, so no EOF
   ever arrived: `/proc/net/unix` showed this process's accepted socket still
   connected, to a peer held by Muse.
2. **The refusal does not exist.** While a connection is held, `proto.c` polls
   that connection and not the listener, so the branch that refuses a second
   connect can never run. A new connect is not refused; it waits in the listen
   backlog, and with a backlog of 1 the third one blocks. The restarted
   launcher's main thread sat in `unix_wait_for_peer` - a blocking `connect` -
   before its first frame. That was the lockup.
3. **Closing the stale holder cured it at once.** Killing Muse delivered the
   EOF, this process logged `launcher disconnected` and then `launcher
   connected`, and the launcher went on to its shelf.

TortOS is fixing its half - a daemon it starts will no longer inherit its
descriptors. That removes this holder. It does not make the premise true: any
process that forks and does not close what it inherits can keep a socket alive,
and a launcher that hangs rather than dies holds one too. The protocol should
not depend on every future child of every future launcher getting that right.

## Options considered

### Option A - make the code match ADR-0009

Poll the listener while connected, accept the second connect, and close it,
which is what the comment in `proto.c` has claimed all along.

Honest, and not enough. The restarted launcher would be refused instead of
blocked, with the stale holder still connected, so it could never reach the
resident at all. Every launch would fall back to the standalone path until
something killed the holder - and the standalone path is itself an alarm, not a
mode.

### Option B - keep the connection, detect staleness

Ping the connected launcher and drop it when it does not answer.

Rejected. A launcher is not always able to answer: it is blocked in a game's
wait loop, a menu's draw, a network call on a worker. Any timeout short enough
to recover quickly is short enough to drop a live launcher that was busy, and
the protocol gains a liveness rule it has never needed.

### Option C - the newest connection wins

A connect while one is held replaces it. The old connection is closed, the new
one gets `READY` with the current `state=`, and the host treats the old one
exactly as it treats a hangup.

This is what Muse's own socket already does, for the same reason: the launcher
that connected last is the one that is alive. There is one launcher per device,
started by one supervisor, so two live launchers competing is not a case that
exists - and if it ever did, the older one would lose its connection visibly
rather than a newer one hanging silently.

## Decision

**Option C.** The listener is polled whether or not a launcher is connected. A
new connection displaces the current one:

- the old connection is closed, and whatever it had half-sent is discarded;
- the new one is told `READY` with `state=` as today, so a launcher restarted
  mid-game still knows the screen is spoken for;
- the host sees `HANGUP` for the old one, so every loop that already knows what
  a vanished launcher means - the in-game menu resumes the game - does exactly
  that, and nothing new has to be taught what "displaced" means.

`state=` on connect, the rest of ADR-0009, and ADR-0008's rule that a dead
launcher cannot end a game are all unchanged.

## Consequences

**Easier.** A launcher restart can never be locked out by a socket something
else is holding, whatever the something is. The failure that happened cannot
recur by any route, not only the one found.

**Harder.** A second process that connects by mistake - a test tool, a
debugging session over ssh - now takes the connection from the launcher rather
than being turned away. The launcher connects again the next time it needs the
resident, which displaces the tool in turn, so the damage is bounded, but a
person poking at the socket on a live device should know they are borrowing
it. Also, the host now has two sources of `HANGUP`, and a log line says which it
was.

**Forecloses:** more than one launcher connected at once. ADR-0009 already
forbade it; this makes the latest one the winner rather than the first.

## Revisit if

- Diatom gains a second kind of client that must stay connected alongside the
  launcher - a debugger, a remote control. One-at-a-time, in any form, is then
  the wrong shape.
- A launcher is found reconnecting in a loop, displacing itself, which would
  mean something in it connects twice.
