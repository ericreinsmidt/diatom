#!/usr/bin/env python3
"""Offline half of the core-facts check - no device needed.

`tools/corefacts.sh --check` re-measures on hardware and is the real proof, but
it needs the Brick plugged in and the user's ROMs present, so it cannot be the
thing that runs on every build. This is what can.

It asserts one thing: **the pinned core set recorded in core-facts.md is still
the pinned core set in CORES.md.** If a core is swapped, every measured row in
that file describes a binary the project no longer runs, and this fails the
moment the manifest changes rather than months later when someone notices an
aspect ratio looks wrong.

That is exactly the failure it was written for. ADR-0015's aspect table was
measured 2026-08-24, and the core selection a day later replaced the cores three
of its six rows had been measured against - Genesis went 1.3333 to 1.5238 when
PicoDrive lost to Genesis Plus GX, SNES 1.3333 to 1.5842 when snes9x 1.63 lost
to snes9x2010, and NES 1.2190 to 1.3061 on a rebuilt FCEUmm. Nothing failed.
Nobody could have known without re-measuring by hand, which finally happened a
day later and only by accident.

Worth recording that the hand audit ALSO got it wrong, in both directions: it
measured a device staging directory that had drifted from CORES.md, so it read
mGBA's rate as 48000 when the pinned build reports 65536, and called the Game
Boy row stale when it was correct. Three of six, not four. That is the argument
for a tool over an audit - the audit was careful and still wrong, because
carefulness cannot verify a hash.

That is all it does, deliberately. A first version also tried to forbid docs
from restating any number this file owns, on the theory that a copied figure
cannot be re-checked. It fired on `50.0070` in a dozen places - which is not a
core's opinion, it is the correct PAL frame rate, a fact about the hardware. A
check that cannot tell a legitimate reference from a stale copy is one people
learn to ignore, and this project has already written that lesson down twice.

So citing this file by link rather than by copying is a **convention**, stated
in the file itself, and it is honest about not being enforced. What IS enforced
is the thing that actually caused the drift: the moment the pinned set changes,
every measurement here is suspect and this says so.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FACTS = os.path.join(ROOT, "docs", "reference", "core-facts.md")
MANIFEST = os.path.join(ROOT, "CORES.md")
DOCS = os.path.join(ROOT, "docs")

# Name and hash, with whatever columns CORES.md carries between them. An earlier
# version pinned the byte count's position and broke the day a license column was
# added - reporting that every core had been "unpinned", which is alarming and
# false. Match the two things that identify a core, not the table's shape.
PIN = re.compile(r"`([a-z0-9_]+)`\s*\|.*?\|\s*`([a-f0-9]{64})`")
FACT_PIN = re.compile(r"^- `([a-z0-9_]+)` `([a-f0-9]{64})`", re.M)


def read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


def main():
    fails = []

    if not os.path.exists(FACTS):
        print("core-facts.md does not exist - run tools/corefacts.sh")
        return 1

    manifest = dict(PIN.findall(read(MANIFEST)))
    facts_txt = read(FACTS)
    recorded = dict(FACT_PIN.findall(facts_txt))

    if not recorded:
        fails.append("core-facts.md records no pinned set; it cannot be checked")

    for core, sha in sorted(recorded.items()):
        want = manifest.get(core)
        if want is None:
            fails.append("core-facts.md measured `%s`, which CORES.md no longer "
                         "pins at all" % core)
        elif want != sha:
            fails.append("`%s` changed since core-facts.md was measured:\n"
                         "        measured against %s\n"
                         "        CORES.md now has %s\n"
                         "        Every row using this core is now a claim about a "
                         "binary nothing runs.\n"
                         "        Re-run tools/corefacts.sh with the device attached."
                         % (core, sha[:16], want[:16]))

    for core in sorted(set(manifest) - set(recorded)):
        fails.append("CORES.md pins `%s` but core-facts.md never measured it" % core)

    for f in fails:
        print("\nFAIL  " + f)

    if fails:
        print("\n%d problem(s). Measurements in docs/ may describe binaries that "
              "are no longer shipped." % len(fails))
        return 1
    print("ok: core-facts.md was measured against the cores CORES.md still pins")
    return 0


if __name__ == "__main__":
    sys.exit(main())
