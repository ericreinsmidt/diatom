#!/usr/bin/env python3
"""Structural checks on docs/scoping-register.md.

The register is the project's resume point: read a section, see what is left.
By 2026-08-25 it could no longer do that. It had grown 418 -> 992 lines in
three days and never once shrunk, because every resolution was APPENDED rather
than substituted:

  - Section 10 opened with five unticked questions and then, twelve lines down,
    a `RESOLVED -> ADR-0006` block ticking the same five.
  - The core-options work created `## 4c. Core options` and left
    `## 12. Core options and config` standing with its stale boxes.
  - Saves did the same thing: `## 5b` records ADR-0016 while `## 9` still asks
    about SRAM write policy and slot convention.

None of that was neglect - the register was edited in 15 of the 16 ADR commits.
It was a habit: writing the resolution is the satisfying part, and deleting the
question it answers feels like discarding information in a project whose whole
discipline is to keep the reasoning. The reasoning lives in the ADR. The
register only needs to say what is left.

So this exists for the same reason `check-seam` does. That rule held because a
build fails when it is broken, not because anyone remembered it. Nothing ever
failed when the register drifted, so it drifted.

Two severities, and the split is deliberate:

  FAIL     structural facts a machine can be certain about. Ordering, a
           resolved section still carrying open questions, an undeclared tag.
  WARN     suspected fossils - an open item that looks answered by a ticked one
           elsewhere. Needs a human to decide, so it never fails a build. A
           check that cries wolf is one people learn to ignore, which is worse
           than no check at all.

  tools/check-register.py            check the working tree
  tools/check-register.py --diff     also enforce substitution over appending
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REG = os.path.join(ROOT, "docs", "scoping-register.md")
ADR_DIR = os.path.join(ROOT, "docs", "decisions")

HEADING = re.compile(r"^## (\d+)([a-z]?)\.\s*(.+?)\s*(?:\*\(.*\)\*)?$")
ITEM = re.compile(r"^- \[( |x)\]\s*(.*)$")
TAG = re.compile(r"\*\*\[([A-Z][A-Z ]*)\]\*\*")
RESOLVED = re.compile(r"RESOLVED|→\s*\[ADR-|-> \[ADR-")
ADR_REF = re.compile(r"ADR-(\d{4})")

# Words that carry no subject information, so two items sharing only these are
# not the same question.
STOP = {
    "about", "after", "against", "already", "another", "anything", "because",
    "before", "being", "below", "between", "both", "cannot", "could", "device",
    "diatom", "does", "doing", "either", "every", "first", "from", "have",
    "here", "into", "just", "like", "measured", "might", "much", "must",
    "never", "only", "other", "presumably", "really", "same", "should", "since",
    "some", "still", "such", "than", "that", "them", "then", "there", "these",
    "they", "thing", "this", "those", "through", "under", "until",
    "what", "when", "where", "which", "while", "will", "with", "would",
    "accepted", "decided", "settled", "resolved", "answer", "answered",
}


def load():
    with open(REG, encoding="utf-8") as f:
        return f.read().split("\n")


def sections(lines):
    """[(number, letter, title, start, end)] in file order."""
    out, cur = [], None
    for i, ln in enumerate(lines):
        m = HEADING.match(ln)
        if m:
            if cur:
                out.append(cur + (i,))
            cur = (int(m.group(1)), m.group(2), m.group(3), i)
    if cur:
        out.append(cur + (len(lines),))
    return out


def items(lines, start, end):
    """[(ticked, text, lineno)] - continuation lines folded into the item."""
    out = []
    i = start
    while i < end:
        m = ITEM.match(lines[i])
        if m:
            text, j = m.group(2), i + 1
            while j < end and lines[j].startswith("      "):
                text += " " + lines[j].strip()
                j += 1
            out.append((m.group(1) == "x", text, i + 1))
            i = j
        else:
            i += 1
    return out


def subject(text, minlen=5):
    """Distinctive tokens: acronyms, and words long enough to mean something.

    Tags come out FIRST. Leaving them in makes every open item share the token
    `open`, every warning fire, and the whole check worthless - which is what
    the first version of this did."""
    text = TAG.sub("", ADR_REF.sub("", text))
    toks = set()
    for w in re.findall(r"[A-Za-z_][A-Za-z0-9_]+", text):
        if w.isupper() and len(w) >= 3:
            toks.add(w.lower())
        elif len(w) >= minlen and w.lower() not in STOP:
            toks.add(w.lower())
    return toks


def same_subject(a, b, floor=2, ratio=0.34):
    """Shared vocabulary as a fraction of the SHORTER item, not of the union.
    A one-line question and a six-line resolution can be about exactly the same
    thing, and Jaccard would score that near zero purely on length."""
    shared = a & b
    if len(shared) < floor or not a or not b:
        return None
    return shared if len(shared) / min(len(a), len(b)) >= ratio else None


def declared_tags(lines):
    """Everything before the first section heading is the header. Bounded that
    way rather than by a line count, which broke the moment the header grew."""
    end = next((i for i, l in enumerate(lines) if l.startswith("## ")), len(lines))
    return set(TAG.findall("\n".join(lines[:end])))


def check_working_tree():
    lines = load()
    secs = sections(lines)
    fails, warns = [], []

    # 1. Section numbering must not go backwards. `## 5` at 353 sitting above
    #    `## 4b` at 471 is how you can tell sections were inserted wherever was
    #    convenient rather than where they belong.
    prev = None
    for num, letter, title, start, _ in secs:
        key = (num, letter or "")
        if prev and key < prev[0]:
            fails.append("section order: ## %d%s. %s (line %d) comes after ## %d%s. %s"
                         % (num, letter, title, start + 1, prev[0][0], prev[0][1], prev[1]))
        prev = (key, title)

    # 2. A section that records a resolution must not still carry open items.
    #    This is the exact signature of the append habit.
    for num, letter, title, start, end in secs:
        res = next((i for i in range(start, end) if RESOLVED.search(lines[i])), None)
        if res is None:
            continue
        stale = [(t, n) for ok, t, n in items(lines, start, end) if not ok and n <= res]
        for text, n in stale:
            fails.append("## %d%s. %s is resolved (line %d) but line %d is still open:\n"
                         "        - [ ] %s"
                         % (num, letter, title, res + 1, n, text[:96]))

    # 3. Tag vocabulary. The header once declared "nothing is decided unless it
    #    says DECIDED" while every resolution since was written as RESOLVED -
    #    the stated vocabulary and the real one drifted apart unnoticed.
    known = declared_tags(lines)
    seen = {}
    for num, letter, title, start, end in secs:
        for _, text, n in items(lines, start, end):
            for t in TAG.findall(text):
                seen.setdefault(t, n)
    for t, n in sorted(seen.items()):
        if t not in known:
            fails.append("tag **[%s]** used at line %d is not declared in the header legend" % (t, n))

    # 4. Two sections on the same subject, which is what happens when new work
    #    gets a new section beside the old one instead of updating it.
    for i, a in enumerate(secs):
        for b in secs[i + 1:]:
            sa, sb = subject(a[2], minlen=4), subject(b[2], minlen=4)
            if len(sa & sb) >= 2 or (sa and sa == sb):
                fails.append("## %d%s. %s and ## %d%s. %s look like the same subject"
                             % (a[0], a[1], a[2], b[0], b[1], b[2]))

    # 5. Every Accepted ADR should be reachable from a ticked item, or the
    #    register does not reflect a decision that has already been made.
    accepted = set()
    for fn in sorted(os.listdir(ADR_DIR)):
        m = re.match(r"(\d{4})-", fn)
        if not m:
            continue
        with open(os.path.join(ADR_DIR, fn), encoding="utf-8") as f:
            if re.search(r"^[-*\s]*\**Status:?\**\s*:?\s*Accepted", f.read(),
                         re.I | re.M):
                accepted.add(m.group(1))
    ticked_adrs = set()
    for num, letter, title, start, end in secs:
        for ok, text, _ in items(lines, start, end):
            if ok:
                ticked_adrs |= set(ADR_REF.findall(text))
    for a in sorted(accepted - ticked_adrs):
        fails.append("ADR-%s is Accepted but no ticked register item points at it" % a)

    # 5b. The ADR index is a second place the same drift showed up: it stopped
    #     at 0012 while 0013-0016 were written, because adding the file is the
    #     work and adding the row is the bookkeeping. Same fix.
    with open(os.path.join(ADR_DIR, "README.md"), encoding="utf-8") as f:
        index = f.read()
    for fn in sorted(os.listdir(ADR_DIR)):
        m = re.match(r"(\d{4})-", fn)
        if m and fn not in index:
            fails.append("%s exists but is not in the decisions/README.md index" % fn)

    # 5c. Settled items must stay short. Fossils were only half the growth:
    #     79 settled items at a mean of 4.5 lines is a third of the file
    #     describing things that are DONE, and open items drown in it. If an
    #     answer needs more room than this, the room is an ADR, a spike or a
    #     discussion log - and if none exists, that is the thing to write.
    SETTLED_MAX = 6
    for num, letter, title, start, end in secs:
        i = start
        while i < end:
            if lines[i].startswith("- [x]"):
                j = i + 1
                while j < end and lines[j].startswith("      "):
                    j += 1
                if j - i > SETTLED_MAX:
                    fails.append("## %d%s. %s line %d: a settled item is %d lines "
                                 "(max %d)\n        %s"
                                 % (num, letter, title, i + 1, j - i,
                                    SETTLED_MAX, lines[i][:88]))
                i = j
            else:
                i += 1

    # 6. Suspected fossils: an open item whose subject is already covered by a
    #    ticked one somewhere else. WARN, not FAIL - "is this the same
    #    question?" is a judgement, and a check that guesses wrong loudly is a
    #    check people stop reading.
    ticked = []
    for num, letter, title, start, end in secs:
        for ok, text, n in items(lines, start, end):
            if ok:
                ticked.append((subject(text), num, letter, title))
    for num, letter, title, start, end in secs:
        for ok, text, n in items(lines, start, end):
            if ok:
                continue
            s = subject(text)
            for ts, tn, tl, tt in ticked:
                shared = same_subject(s, ts)
                if shared and (tn, tl) != (num, letter):
                    warns.append("line %d in ## %d%s. %s may already be answered by "
                                 "## %d%s. %s (shared: %s)\n        - [ ] %s"
                                 % (n, num, letter, title, tn, tl, tt,
                                    ", ".join(sorted(shared)[:4]), text[:88]))
                    break
    return lines, secs, fails, warns


def check_diff(ref):
    """Substitution, not appending: a commit that records a resolution has to
    remove the question it answers. This is the rule the whole file exists to
    hold, and it is the only one that can be enforced on the change rather than
    on the state."""
    try:
        out = subprocess.run(["git", "diff", ref, "--", REG], cwd=ROOT,
                             capture_output=True, text=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        return ["cannot read the diff against %s (%s)" % (ref, e)]
    added = [l for l in out.split("\n") if l.startswith("+") and not l.startswith("+++")]
    removed = [l for l in out.split("\n") if l.startswith("-") and not l.startswith("---")]
    if not added:
        return []
    if any(RESOLVED.search(l) for l in added) and not removed:
        return ["this change records a resolution but removes nothing from the "
                "register.\n        A resolution REPLACES the question it "
                "answers - the reasoning belongs in the ADR."]
    return []


def main():
    lines, secs, fails, warns = check_working_tree()

    if "--diff" in sys.argv:
        i = sys.argv.index("--diff")
        ref = sys.argv[i + 1] if len(sys.argv) > i + 1 else "HEAD"
        fails += check_diff(ref)

    total_open = total_done = 0
    print("register: %d lines, %d sections" % (len(lines), len(secs)))
    for num, letter, title, start, end in secs:
        its = items(lines, start, end)
        o = sum(1 for ok, _, _ in its if not ok)
        d = sum(1 for ok, _, _ in its if ok)
        total_open += o
        total_done += d
        if its:
            print("  %-5s %-46s %2d open  %2d settled"
                  % ("%d%s." % (num, letter), title[:46], o, d))
    print("  %-52s %2d open  %2d settled" % ("TOTAL", total_open, total_done))

    for w in warns:
        print("\nWARN  " + w)
    for f in fails:
        print("\nFAIL  " + f)

    if fails:
        print("\n%d structural problem(s). The register cannot be trusted to say "
              "what is left." % len(fails))
        return 1
    if warns:
        print("\nok, with %d item(s) worth a look. Warnings do not fail." % len(warns))
    else:
        print("\nok: the register is internally consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
