# 0026. Achievements on the launcher protocol

- **Status:** Proposed
- **Date:** 2026-08-29
- **Supersedes:** -
- **Superseded by:** -

Extends [ADR-0009](0009-launcher-protocol.md) (the protocol) and
[ADR-0020](0020-shared-state-plane.md) (the state plane), and pays off the
piece [ADR-0025](0025-achievements-belong-to-the-frontend.md) deferred:
*"the launcher sends a condition set at RUN, Diatom reports unlocks. That is an
extension of ADR-0009 and wants its own ADR once the shape is known, not a
guess here."* The shape is now known.

## Context

ADR-0025 put evaluation in Diatom because conditions compare against the
previous frame and the launcher's loop is 10Hz against a 60Hz core. That
settled *where* the work happens and left three things to cross the seam.

**Which console.** A RetroAchievements address is an offset into a space
defined per console in `rc_consoles.h`. On the NES, `0x06f3` is work RAM; on
the Mega Drive the same number means something else, and past `0x8000` it is
not memory at all. Nobody on Diatom's side knows which console a game is:
libretro has no call that asks a core, the core does not think in
RetroAchievements' terms, and Diatom deliberately has no library, no
`systems.cfg` and no concept of a system (ADR-0016). The launcher has all
three.

**The set.** Measured 2026-08-29 across 428 achievements from four sets:

| set | achievements | median condition | longest |
|---|---|---|---|
| Blaster Master | 27 | 46 | 124 |
| The Legend of Zelda: A Link to the Past | 110 | 101 | 6,969 |
| Super Mario 64 | 166 | 118 | 8,368 |
| Gran Turismo 2 | 125 | 113 | **30,897** |

*(Titles as RetroAchievements gives them. An earlier revision of this table
named two of these from memory - "Super Metroid" and "Mega Man 2" - which were
game ids I had labelled without checking. The numbers were always the response's
own; the names were not, and were wrong.)*

Diatom's protocol line buffer is **4096 bytes**, and `proto.c` drops anything
longer with a warning. A third of these sets contain at least one achievement
that would not fit.

**Unlocks, back out.** Which is the easy direction.

One timing fact shapes the answer as much as the sizes do: **the set arrives
over the network.** On a handheld that means seconds, and on a handheld whose
WiFi is off it means never. A game must not wait for it.

## Options considered

### Option A - the set inline on the socket

One achievement per line, the way `INPUTS` enumerates buttons. Fits the
existing shape and needs no file.

Costs: 30,897 characters against a 4096-byte buffer. Raising the buffer means
choosing a number, and the table above is exactly the evidence that any number
chosen before measuring would have been wrong - the natural guesses (1KB, 4KB,
8KB) all fail, and they fail on the hardest achievements in the hardest games.
Worse, they fail **silently**: the line is dropped, the achievement is never
watched, and from the player's side that is indistinguishable from one they
have not earned. That is the precise failure mode ADR-0025 exists to prevent.

### Option B - the set as a file, the path over the socket

ADR-0016 already has the launcher passing explicit paths for save states and
previews, so a path is a pattern here rather than an exception. No ceiling, and
the socket stays a control channel.

Costs: a file on disk, and a format that has to be written down.

### Option C - Diatom fetches its own set

Foreclosed by ADR-0025, which draws exactly one line: **Diatom gains no network
access.**

### And, separately: infer the console or be told it

Inferring means a table in Diatom mapping core filenames to RetroAchievements
console ids. That is a library, which the frontend has no business having, and
it is wrong on its own terms: `genesis_plus_gx` is three consoles and `mgba` is
two, so the core cannot answer the question even in principle.

## Decision

**Option B, and the console is declared.**

### Launcher to Diatom

| Message | Meaning |
|---|---|
| `RUN … console=<id> cheevos=<path>` | both optional; a set named here is watched from the first frame |
| `CHEEVOS` | query |
| `SETCHEEVOS path=<file> [console=<id>]` | load a set, replacing whatever is loaded; an empty path unloads |

`SETCHEEVOS` is not a convenience. It is the normal path whenever the set is
still downloading when the player presses A, and `cheevos=` on `RUN` is the
optimization for a set already on disk - which matters, because an achievement
can fire in the first seconds of a game.

### Diatom to launcher

| Message | Meaning |
|---|---|
| `CHEEVOS console= count= unlocked= memory= spans=` | the reply, and unsolicited when the set changes |
| `CHEEVO id= state=active\|primed\|paused\|unlocked\|disabled` | one per achievement in a reply, **and** unsolicited the moment one fires |

Same verb for the reply and the event, per ADR-0020, so the launcher writes one
parser rather than two. `memory=` and `spans=` are there because "the set
loaded" and "there is memory under it" are different facts and a launcher
showing achievements needs to know it is not showing a lie.

### The file

```
# comments and blank lines are skipped
76195	0xH06f3=0_0xH0400=3_0xH00ba=0_0xH06f0<d0xH06f0
```

`<id>` TAB `<condition>`, one per line. Anything past a second tab is ignored
and is room for a title later. **Not JSON**: RetroAchievements' wire format is
the launcher's problem, and a parser here would be a second place that has to
track their schema as it changes.

### What Diatom is not told, and cannot be

No account, no title, no points, no icon, and no record of what the player has
already earned. **The launcher sends only what it wants watched** - an
achievement already earned is simply left out of the file. Diatom has nowhere
to keep that, no way to check it, and no business trying.

### What is reported, and what is not

`TRIGGERED` and `DISABLED`. `DISABLED` means the achievement referenced memory
this core does not map; without a report that is, again, indistinguishable from
unearned.

`PRIMED` and `UNPRIMED` are **not** reported yet. They would let a launcher
show "one condition away", but they are edge events on game state and can
arrive often. Adding an event later costs nothing, because ADR-0009 promises
unknown verbs are ignored. Removing chatter a launcher has come to depend on
costs a great deal.

### Where the frame call goes

Immediately after `retro_run`, before anything else can touch memory. That one
line is the whole of ADR-0025 made real.

**Progress in flight is discarded on a state load and on reset**, because hit
counts and the previous frame every delta measures against belong to a timeline
that no longer exists. The call lives inside `diatom_state_load` rather than at
its three call sites, so a fourth call site inherits the fix instead of having
to remember it.

## Consequences

**Measured:** 100 achievements of mixed shape cost 5.0 microseconds per frame
on the development host - 0.03% of a frame. **The device number is not
measured and is the one that decides anything**; the same is true of the RSS
budget, which ADR-0025's revisit clause now names for that reason.

The loader has no line-length limit at all. It reads with `getline`, which is
one allocation per load and the honest answer to a question whose measured
range spans three orders of magnitude.

`test/stateplane.py` grew a section, since achievements are on the plane it
already drives and a second harness would drift from the first. Writing it
found a real fixture bug: the stub core's frame counter carried across games,
because cores stay resident (ADR-0006), so every fixture built on that counter
meant something different on the second game than on the first.

The stub core now exposes system RAM, so the achievement path can be exercised
end to end with no emulator present.

**Not addressed:** hardcore mode, leaderboards, rich presence. rcheevos
supports all three and the vendored runtime already carries the code, so each
is a protocol decision rather than an implementation one. None has a launcher
asking for it yet.

## Revisit if

- A launcher wants a progress indicator, which is `PRIMED`/`UNPRIMED` and a
  decision about how noisy they turn out to be on real games.
- Leaderboards arrive. They need a value out, not a boolean, and `rc_runtime`
  already tracks them.
- Unlocks need to survive a Diatom restart. Today they live in the runtime and
  die with the game, which is correct while the launcher owns the account -
  and would stop being correct if it ever did not.
