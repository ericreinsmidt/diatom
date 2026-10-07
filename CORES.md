# Cores the test matrix was verified against

Pinned 2026-08-26. Generated from the table in
`tools/fetch-cores.sh`, which is where the pin lives; this file is the
readable copy of it, and the script fails rather than rewriting it.

**Not part of diatom.** These are third-party binaries; diatom neither
ships nor depends on them, and loads whatever core it is handed. This file
exists so the measurements in `docs/` name the exact bytes that produced
them. The binaries themselves are gitignored.

Source: `https://buildbot.libretro.com/nightly/linux/aarch64/latest` - libretro's own buildbot. The path is **unpinned**;
these hashes are the pin.

Licenses are pinned with the hashes and re-checked on every run against
libretro's core-info. The set is **not uniformly GPL** and the
differences matter to anyone shipping an image.

| Core | License | Bytes | sha256 |
|---|---|---|---|
| `fceumm` | GPLv2 | 4451672 | `1b13b00d4680394dad8000d5175f97be727107e0945bc9b412da91d70c07b267` |
| `snes9x2010` | Non-commercial | 2859704 | `3933890f520abb9dbb0e5276460785b20ce54d25f552b369cafeca270b9dd44c` |
| `mgba` | MPLv2.0 | 3280408 | `abde7a0764f08fa0cc2c7d3d9a29b9d1245a9f3b7df0e7a594b74df642ee53c6` |
| `genesis_plus_gx` | Non-commercial | 12589216 | `3673a22b906509461e23a5a118b1d1bec15cbda105f260cbcbc08a16b2124e48` |
| `mednafen_pce_fast` | GPLv2 | 4465432 | `aca90a14b18108c86398da2267ef40d5145eaddbc1c1b310614d745b258552b1` |

Each core also reports its own name and version, which diatom logs at
load and which usually carries an upstream git hash. Those strings are
recorded alongside the measurements that used them.

## C++ cores do not run on the Brick as fetched

Measured 2026-08-24/25. `snes9x` (mainline), `gambatte`, `mesen-s` and
`mednafen_supafaust` all fail to `dlopen` with
`GLIBCXX_3.4.29 not found`. The buildbot builds C++ cores against GCC 11;
the Brick ships `libstdc++.so.6.0.28`, which tops out at `GLIBCXX_3.4.28`.
One version short.

Checking `GLIBC_` alone does not catch this - the C++ runtime versions
its symbols separately as `GLIBCXX_`, and a core can be clean on one and
fail on the other.

The fix is already to hand: the toolchain container from ADR-0012 has
GCC 10.2.1, whose cross `libstdc++.so.6.0.28` provides exactly
`GLIBCXX_3.4.28` - the same version the device ships. A C++ core built
there runs unmodified, with provenance pinned to an upstream commit
rather than to a nightly. Bundling a newer `libstdc++` beside the cores
would also work and is what other firmwares do.

C cores - fceumm, picodrive, genesis_plus_gx, mgba, mednafen_pce_fast -
are unaffected and load as fetched.
