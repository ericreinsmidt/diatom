# Third-party notices

Diatom's own code is under the Mozilla Public License 2.0: its own files stay
open, and the license reaches no further than them, so Diatom escapes GPL
inheritance either way, which is a founding goal of the project.

## `src/libretro.h`

The libretro API header, vendored so the build is self-contained.

- **Copyright (C) 2010-2024 The RetroArch team**
- **MIT-style permissive license**, and the header states that its license
  statement *"only applies to this libretro API header (libretro.h)"*.

That scoping is deliberate on libretro's part and is what makes the whole
arrangement work: any-license frontends may host any-license cores, because the
interface between them carries no copyleft.

Full text is at the top of the file.

## `vendor/rcheevos/`

RetroAchievements' implementation of their achievement condition language,
vendored so the build stays self-contained.

- **Copyright (c) 2018 RetroAchievements.org**
- **MIT licensed**, full text in `vendor/rcheevos/LICENSE`.

MIT is the whole reason this is possible rather than a problem: it fits beside
diatom's own license, so the project's founding goal - escaping GPL inheritance - is
untouched. The networking halves (`rc_client`, `rapi`, `rurl`) are deliberately
not vendored; `vendor/rcheevos/README.md` lists every omission and why.

## Cores

**Diatom ships no cores and has no core list.** It loads whatever shared library
it is handed at runtime. Cores keep their own licenses - commonly GPL - and are
the responsibility of whoever distributes them.

`test/stubcore.c` is diatom's own code: a libretro core that is not an emulator,
so the frontend can be exercised with no third-party binary present.
