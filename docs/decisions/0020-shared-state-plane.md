# 0020. The protocol grows a state plane, with one owner per item

- **Status:** Proposed
- **Date:** 2026-08-26
- **Supersedes:** -
- **Superseded by:** -

Extends [ADR-0009](0009-launcher-protocol.md), which is unchanged. Builds the
part [ADR-0019](0019-input-mapping-and-remapping.md) deliberately left unbuilt.

## Context

Three open register items turned out to be one missing thing.

1. **`SET_INPUT_DESCRIPTORS` is accepted and discarded.** The core tells us
   *"B = Jump"* for this specific game, free, on every load, and we throw it
   away. It is exactly what makes a remap screen readable.
2. **ADR-0019's remap table has no way to arrive.** The decision says the host
   owns canonical → retropad as data and the launcher owns the configuration.
   Nothing carries it.
3. **Volume and brightness set during a game do not survive the exit.**

ADR-0009 built a protocol of **commands and events**: `RUN`, `STOP`, `QUIT` in;
`RUNNING`, `EXIT`, `ERROR` out. All three items above are neither. They are
**state that both sides need to agree about**, and the protocol has no discipline
for that - except `OPTIONS`/`SETOPT`, which is already a state pattern that was
never named as one.

Solved separately they become three unrelated messages designed months apart.
That is the accretion ADR-0009 was written to escape, and it would be a poor joke
to reproduce it inside the replacement.

### What the third item actually turned out to be

The register said levels "do not survive the exit", implying they are discarded.
Measurement on 2026-08-26 found something worse and more instructive. The
launcher's brightness ladder is eleven geometric rungs
(`1 8 16 32 48 72 96 128 160 192 255`); Diatom's was twenty linear steps.
**Almost none of Diatom's values existed on the launcher's ladder at all.**

So the two sides did not disagree about a number. They disagreed about what the
numbers *were*. Any design that ships a bare level index across this socket
inherits that bug, whichever ladder it picks. See
[the measurement](../spikes/2026-08-26-backlight-floor.md).

## Decision

**Name the state plane, give it one shape, and state an owner for every item.**

### The shape

For each piece of shared state `X`:

| Message | Direction | Meaning |
|---|---|---|
| `X` | launcher → Diatom | query |
| `X …` | Diatom → launcher | the value: as a reply, **and** unsolicited when Diatom changes it |
| `SETX …` | launcher → Diatom | write, whole-value and atomic |

The same verb carries the reply and the event, so the launcher writes one parser
per state rather than two. `OPTIONS` already works this way; this generalises it
rather than inventing anything.

### The four states and who owns each

| State | Defined by | Written by | Diatom emits unsolicited |
|---|---|---|---|
| Core options | the core | launcher | no |
| Input labels | the core, per game | nobody, read-only | no |
| Button map | launcher | launcher | no |
| **Levels** (volume, brightness) | **contested** | both | **yes** |

Only the last row needed a decision, and it is this:

> **The port owns levels while a game runs. The launcher owns them the rest of
> the time. `RUNNING` transfers ownership, `EXIT` returns it.**

That is not a compromise, it follows from ADR-0009's display rule. The launcher
is not drawing during a game, so it *cannot* present a volume UI, which is why
the bar lives in the port at all. The moment it takes the screen back, it is the
only side that can.

### Input labels

```
launcher → INPUTS
Diatom   → INPUTS count=3
           INPUT id=b     label=Jump
           INPUT id=a     label=Fire
           INPUT id=start label=Pause
```

**`id=` names a canonical Diatom button, never a retropad id.** The core labels
retropad ids; Diatom already knows the map, so it resolves the two and reports
what a person would need to read: *the button the device calls X currently does
Jump*. Labels therefore track the active remap for free, and retropad ids stay
out of the protocol exactly as `libretro.h` stays out of the port (ADR-0007).

Cores may call `SET_INPUT_DESCRIPTORS` more than once and late. Query-on-demand
rather than push-on-load makes that a non-issue.

### Button map

```
launcher → SETMAP map=x:b,y:a       identity for anything unlisted
launcher → SETMAP map=identity      clear
launcher → MAP                      query
Diatom   → MAP map=x:b,y:a
```

One field, one message, applied whole. Per-binding messages would allow a
half-applied map if a launcher died mid-sequence, and a half-applied map is
unplayable in a way that is hard to diagnose.

`none` is a legal target (`x:none`) so a button can be unbound. **`menu` is
rejected on either side of a pair**, which is ADR-0019's fourth rule appearing
where it can actually be enforced.

### Levels

```
Diatom   → LEVEL kind=brightness index=7 count=12
launcher → LEVELS                                       query both
launcher → SETLEVEL kind=brightness index=7 count=12
```

**`count=` always travels with `index=`, in both directions.** This is the whole
fix for the ladder mismatch, and it is one rule: *a level is a fraction, not a
number*. Neither side may assume the other's scale, and a receiver whose own
count differs scales proportionally and lands on its own nearest rung.

Raw device units never cross the socket. The port's ladder stays inside the port,
where it is hardware truth (ADR-0019 rule 1).

### How the port reports a change

The port is not allowed to know the protocol exists, so it cannot send anything.
The host **polls** it:

```c
/* Levels the user changed with the device's own keys, if this port has any.
 * Returns false if it has none - the desktop port never has. */
bool diatom_port_level_get(int kind, int *index, int *count);
void diatom_port_level_set(int kind, int index, int count);
```

Once per frame, alongside the input bitfield it already polls, and the host emits
`LEVEL` when a value differs from the last one it sent. No new callback
direction, no reentrancy, no work in the port's key handler beyond what it
already does.

### Version

`READY proto=2`. ADR-0009 already promises unknown verbs and keys are ignored, so
an old launcher against a new Diatom is unaffected; `proto=` is what lets a new
launcher discover that the state plane is absent rather than guessing from
silence.

## Consequences

**Easier.** Three items collapse into one shape with one parser. A remap screen
becomes buildable: the labels, the table and the transport all exist. Levels stop
being silently wrong across the exit.

**Harder.** Diatom emits unsolicited messages during a game for the first time,
so the launcher's supervision loop must tolerate traffic it did not ask for -
ADR-0009 already shapes that loop as `poll()`, so this is a smaller change than
it sounds. And `proto=2` is a real compatibility surface now.

**A bug this exposes.** `cb_input_state` returns on the *first* canonical button
whose map matches the queried id. Under an identity map no two buttons share a
target, so this has always been correct. Remapping makes sharing legal - map both
X and Y to B and only one of them fires. It has to become an OR across all
canonical buttons mapping to that id. This is not a consequence of the decision
so much as something the decision made visible, which is the useful kind.

**Not foreclosed.** Per-game and per-system maps need nothing here; they are
different tables the launcher chooses between (ADR-0019 rule 3). Analog stays out
(ADR-0003). Multiple controller ports are not addressed and would need `port=`
added to `INPUT` and `SETMAP`, which the ignore-unknown-keys rule permits without
a version bump.

**Deliberately not done.** Diatom does not persist any of this. It holds no
config file, learns nothing across a run, and asks for the map every time. The
launcher owns storage, and a resident frontend that quietly accumulated its own
preferences would be a second source of truth for settings the launcher already
has a UI for.

## Revisit if

- A launcher wants to change the map *while a game runs* and expects the labels
  to update without re-querying, which would argue for `INPUTS` becoming
  unsolicited on `SETMAP` and is deliberately not done now.
- Levels need to be something other than an index into a ladder - a true
  percentage, or a dB figure for volume - which would mean the fraction rule is
  too weak rather than merely abstract.
- A second state item turns out to be genuinely contested the way levels are.
  One such row is a decision; three would mean the ownership model is wrong.
