# Third-party notices

Diatom's own code is intended to be permissively licensed (see ADR-0002 for why
that matters - escaping GPL inheritance is a founding goal of the project).

## `src/libretro.h`

The libretro API header, vendored so the build is self-contained.

- **Copyright (C) 2010-2024 The RetroArch team**
- **MIT-style permissive license**, and the header states that its license
  statement *"only applies to this libretro API header (libretro.h)"*.

That scoping is deliberate on libretro's part and is what makes the whole
arrangement work: any-license frontends may host any-license cores, because the
interface between them carries no copyleft.

Full text is at the top of the file.

## Cores

**Diatom ships no cores and has no core list.** It loads whatever shared library
it is handed at runtime. Cores keep their own licenses - commonly GPL - and are
the responsibility of whoever distributes them.

`test/stubcore.c` is Diatom's own code: a libretro core that is not an emulator,
so the frontend can be exercised with no third-party binary present.
