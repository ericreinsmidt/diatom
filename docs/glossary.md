# Glossary

Shared vocabulary. Added to as terms come up. Terms marked **⚠ overloaded** have
caused, or will cause, real confusion - use the disambiguated form.

---

**Diatom** - the name of this project (ADR-0004). The **host**: a minimal
libretro frontend for low-power Linux handhelds. A diatom is a single-celled
alga with a silica shell - individually invisible, and the material that
accumulates into chert. Not to be confused with `diatom-lang`, an unrelated
programming language.

**core** - ⚠ overloaded. In libretro, a core is an emulator or game engine
compiled as a shared library implementing the libretro ABI
(`mgba_libretro.so`). It is *not* "the core of our system". Never use the word
for our own internals; say "engine", "runtime", or the specific module name.

**frontend** - The program that hosts cores: loads them, feeds them input,
takes their video and audio, and puts them on screen. This is what we're
building. RetroArch and minarch are both frontends.

**libretro** - The plugin ABI that lets any frontend host any core. Defined by
a single header, `libretro.h`, which is permissively licensed specifically so
that frontends and cores may carry different licenses.

**libretro-common** - A shared utility library from the libretro project. Used
by RetroArch and by minarch, but a separate component from either. Gitignored
in NextUI and cloned unpinned at build time - one of the reproducibility
problems motivating this project.

**minarch** - MinUI's minimal libretro frontend, carried forward by NextUI.
GPL-3.0. What PlayOS currently patches (nine override files) and rebuilds. The
thing we may be replacing.

**MinUI → NextUI** - MinUI (MIT) is the original minimal handheld launcher;
NextUI (GPL-3.0) is its continuation. PlayOS builds minarch from NextUI source.

**RA** - ⚠ overloaded. In NextUI's source, `ra_*.c` means **RetroAchievements**,
not RetroArch. Easy and costly misread.

**RetroArch** - The large general-purpose libretro frontend. We share its ABI
and its cores; we share none of its code.

**port** - ⚠ overloaded. In this project: the platform backend layer (video,
audio, input, system services for one device). In libretro's input API, "port"
also means a controller slot. Say "platform port" or "controller port".

**residency** - Holding a core loaded in memory between games so the next launch
skips `dlopen` and initialization. PlayOS's resident mode took launch from
~1100ms to ~200ms with three cores. The open question is whether it survives a
larger core set on 1GB.

**RETRO_ENVIRONMENT** - The callback through which cores ask the frontend for
capabilities and settings. Its long tail is the main reason writing a *general*
frontend is hard - and bounding it to a known core set is what makes writing a
*personal* one tractable.

**seam** - A boundary where one implementation can be swapped for another.
Per register §0, a boundary with only one implementation behind it is not a
seam, it's overhead.

**DRC (dynamic rate control)** - Nudging the audio resample ratio slightly to
keep the output buffer near half full, absorbing drift between the core's frame
rate and the device's actual audio clock. The mature answer to A/V sync.

**tg5040** - The platform identifier for the TrimUI Brick / Smart Pro family,
used throughout NextUI and PlayOS. Also the name of the toolchain container
image.

**ADR** - Architecture Decision Record. One decision per file: context, options,
choice, consequences. Immutable once accepted; superseded rather than edited.
