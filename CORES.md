# Cores the test matrix was verified against

Pinned 2026-11-04. Generated from the table in
`tools/fetch-cores.sh`, which is where the pin lives; this file is the
readable copy of it, and the script fails rather than rewriting it.

**Not part of Diatom.** These are third-party binaries; Diatom neither
ships nor depends on them, and loads whatever core it is handed. This file
names the exact bytes it is tested against, the same TortOS ships. The
binaries themselves are gitignored.

Source: `https://github.com/ericreinsmidt/scutes/releases/download/v1` - scutes' v1 release, which can't change. The
hashes are the pin.

Licenses are pinned with the hashes and re-checked on every run against
libretro's core-info. The set is **not uniformly GPL** and the
differences matter to anyone shipping an image.

| Core | License | Bytes | sha256 |
|---|---|---|---|
| `fceumm` | GPLv2 | 4451672 | `1b13b00d4680394dad8000d5175f97be727107e0945bc9b412da91d70c07b267` |
| `snes9x2010` | Non-commercial | 2859704 | `3933890f520abb9dbb0e5276460785b20ce54d25f552b369cafeca270b9dd44c` |
| `mgba` | MPLv2.0 | 2540864 | `1c180827774f1941565ac82a173633edf4553ac2809a4851c9eee627f2775ff9` |
| `genesis_plus_gx` | Non-commercial | 12589216 | `3673a22b906509461e23a5a118b1d1bec15cbda105f260cbcbc08a16b2124e48` |
| `mednafen_pce_fast` | GPLv2 | 4465432 | `aca90a14b18108c86398da2267ef40d5145eaddbc1c1b310614d745b258552b1` |
| `mednafen_ngp` | GPLv2 | 375872 | `a2015668f9a9403b8bf6941b550fae2c618f37b79173e8ba27c95b95f96bdd99` |

Each core also reports its own name and version, which Diatom logs at
load and which usually carries an upstream git hash.

## C++ cores on TrimUI's firmware

Measured 2026-08-24/25, on the Brick's TrimUI firmware. `snes9x` (mainline),
`gambatte`, `mesen-s` and `mednafen_supafaust` all failed to `dlopen` with
`GLIBCXX_3.4.29 not found`. The buildbot builds C++ cores against GCC 11;
that firmware shipped `libstdc++.so.6.0.28`, which tops out at
`GLIBCXX_3.4.28`. One version short.

Checking `GLIBC_` alone does not catch this - the C++ runtime versions
its symbols separately as `GLIBCXX_`, and a core can be clean on one and
fail on the other.

plastron, which both devices run now, ships `libstdc++.so.6.0.34` (GCC
15.2, up to `GLIBCXX_3.4.34`), so the runtime no longer stands in their
way. They have not been measured on it.

C cores - fceumm, picodrive, genesis_plus_gx, mgba, mednafen_pce_fast -
are unaffected and load as fetched. `mednafen_ngp` is C++ but asks only
for `GLIBCXX_3.4`, so it loaded on TrimUI's firmware too.
