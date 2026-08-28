# 0012. Build our own toolchain; depend on nothing from NextUI or MinUI

- **Status:** Accepted
- **Date:** 2026-08-24
- **Supersedes:** -
- **Superseded by:** -

## Context

The desktop port builds with the host compiler. The device port needs a
cross-toolchain, and the obvious shortcut is the one PlayOS already uses:
`ghcr.io/loveretro/tg5040-toolchain:latest`.

Three reasons that is the wrong default:

1. **It is a NextUI artifact.** The working agreement states Diatom is not a
   minarch fork, a NextUI component or a MinUI derivative. Taking their
   toolchain re-establishes exactly the dependency the project exists to remove,
   in the one place that is hardest to notice later: the build.
2. **It is pinned to `:latest`.** Unpinned upstream is the specific failure that
   motivated this project - PlayOS's docs claim a `v6.11.2` baseline while its
   build reads whatever is in a working directory. Adopting `:latest` would
   reproduce it.
3. **It is named for a different device.** See the naming correction below.

**Measured on a TrimUI Brick running firmware 1.1.1 (the current release), over
USB ADB, 2026-08-24:**

| | |
|---|---|
| CPU | Cortex-A53 (`CPU part 0xd03`) |
| RAM | 998,332 kB, **no swap** |
| Kernel | 4.9.191 |
| glibc | **2.33** (`/lib/libc.so.6 -> libc-2.33.so`) |
| SDL2 | **`SDL-2.30.8-no-vcs`** at `/usr/trimui/lib/`, with 10 PowerVR/GE8300 references |
| Headers | none on device |

**TrimUI's own SDK is older than the device it targets.** `toolchain_sdk_smartpro`
has exactly one release, `20231018`, shipping `aarch64-linux-gnu-7.5.0-linaro`,
a `tg5040` sysroot, and **SDL2 2.26.1**. The device runs 2.30.8. Building against
the SDK would be a downgrade.

The device's SDL2 requires at most `GLIBC_2.29`, so anything built against a
glibc no newer than 2.33 will load.

## The naming correction

Per TrimUI's own repositories, the model codes are:

```
TG2040  TRIMUI Smart          TG3040  TRIMUI Brick
TG4040  Brick Pro             TG5040  Smart Pro
TG5050  Smart Pro S
```

**`tg5040` is the Smart Pro. The Brick is TG3040.** MinUI and NextUI use
`tg5040` as an umbrella label across Brick, Smart Pro and Smart Pro S because one
build serves all three on the shared Allwinner A133. That is reasonable for them
and misleading here: it names one device and covers three.

Diatom's device port is therefore **`brick`**, not `tg5040`. Plain product name,
consistent with `desktop` and a future `miniloong`, with `TG3040` stated in the
file header. Brick Pro (TG4040) is a physically different device and would be a
sibling port, not an ambiguity.

## Options considered

### Option A - use `ghcr.io/loveretro/tg5040-toolchain`
Zero setup, SDL2 prebuilt for the device. Rejected: it is a NextUI artifact, it
is unpinned, and its SDL2 provenance is theirs rather than the platform's.

### Option B - use TrimUI's SDK
The manufacturer's own, which is the right instinct. But it is a single 2023
release shipping GCC 7.5.0 and SDL2 2.26.1, both older than what the device now
runs. Adopting it would mean building against a version the hardware has moved
past.

### Option C - our own container, platform libraries from the device
A Debian base whose glibc is no newer than the device's, a stock
`aarch64-linux-gnu` cross-compiler, SDL2 headers matched to the device's version
from upstream, and link against the libraries the firmware actually ships.

## Decision

**Option C.**

- **Base:** `debian:bullseye-slim`, glibc **2.31**, verified. Comfortably below
  the device's 2.33, so binaries load; new enough for a usable compiler.
- **Compiler:** stock `aarch64-linux-gnu` cross-toolchain from the distribution.
- **Headers:** SDL2 **2.30.8** from upstream `libsdl-org/SDL`, version-matched to
  the device.
- **Libraries:** pulled from the device itself, `/usr/trimui/lib`. TrimUI's own
  GE8300-patched builds, which is what will be there at runtime.
- **Nothing from NextUI, MinUI, LoveRetro or minarch, at any stage.**

The image is pinned by digest, never `:latest`.

The sysroot is **not committed**: device libraries are third-party binaries and
upstream headers are fetched. A script regenerates it from a connected device
plus an upstream tag, which keeps provenance explicit rather than vendored.

## Consequences

**Easier:** The build has no relationship to any other firmware project.
Provenance of every input is stated. Version-matching SDL2 to the device removes
a class of "works here, not there" bug, and using the firmware's own libraries
means the GPU patches are whatever the device actually has.

**Harder:** We maintain a container image where one existed ready-made, and the
sysroot needs a connected device to regenerate. Slower to get to a first build
than taking the shortcut.

**Accepted risk:** SDL2 headers come from upstream while the library is TrimUI's
patched build of the same version. Patches are implementation-level and 2.x holds
its public API, so the headers should describe it accurately - but this is an
assumption, not a verified fact, and a link error is how it would surface.

## Revisit if

- TrimUI publishes an SDK newer than the firmware, making it the better source;
  or
- the device's glibc moves past what the chosen base can target; or
- the upstream-headers-against-patched-library assumption produces a real defect.
