#!/bin/sh
# Regenerates the Brick sysroot from first sources - ADR-0012.
#
# The sysroot is deliberately NOT committed: the libraries are TrimUI's
# binaries and the headers are upstream SDL's, so the repository records how
# to obtain them rather than a copy of them. This script IS that record.
#
#   Libraries: pulled over ADB from the device's own /usr/trimui/lib, plus
#              the dependency closure needed to satisfy the linker. What links
#              is byte-identical to what runs.
#   Headers:   upstream libsdl-org/SDL release 2.30.8, version-matched to the
#              library the firmware ships (SDL-2.30.8-no-vcs, verified
#              2026-08-24 on firmware 1.1.1). Pinned by sha256.
#
# Needs: a connected Brick (adb), curl, and the diatom-brick-toolchain image
# (docker), whose readelf resolves the dependency closure.
#
#   tools/fetch-brick-sysroot.sh [sysroot-dir]      default: sysroot/brick
set -eu

SDL_VERSION=2.30.8
# Recorded 2026-08-24 from the libsdl-org/SDL release asset, trust on first
# use. Guards against a changed tarball, not against a malicious first fetch.
SDL_SHA256=380c295ea76b9bd72d90075793971c8bcb232ba0a69a9b14da4ae8f603350058
SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL2-${SDL_VERSION}.tar.gz"
IMAGE=diatom-brick-toolchain

SYSROOT=${1:-sysroot/brick}
LIBDIR="$SYSROOT/usr/trimui/lib"
INCDIR="$SYSROOT/usr/include/SDL2"
CACHE="$SYSROOT/.cache"

say()  { printf 'sysroot: %s\n' "$*"; }
fail() { printf 'sysroot: ERROR: %s\n' "$*" >&2; exit 1; }

adb get-state >/dev/null 2>&1 || fail "no device over adb"
docker image inspect "$IMAGE" >/dev/null 2>&1 \
    || fail "toolchain image missing; build it first:
  docker build -f tools/brick-toolchain.Dockerfile -t $IMAGE tools"

mkdir -p "$LIBDIR" "$INCDIR" "$CACHE"

# --- libraries, from the device -------------------------------------------

# adb pull follows symlinks, so pulling the SONAME yields a real file under
# the SONAME's own name, which is exactly what the linker wants to find.
pull_lib() {
    adb pull "$1" "$LIBDIR/" >/dev/null 2>&1
}

say "pulling libSDL2 from device /usr/trimui/lib"
pull_lib /usr/trimui/lib/libSDL2-2.0.so.0 || fail "could not pull libSDL2"
ln -sf libSDL2-2.0.so.0 "$LIBDIR/libSDL2.so"   # -lSDL2 resolves through this

# Dependency closure. The linker wants every DT_NEEDED of every library it
# links resolvable, or it cannot verify the link. glibc members are excluded:
# they come from the cross toolchain at link time (its 2.31 is the floor the
# binary demands; the device's 2.33 satisfies it at runtime - ADR-0012).
GLIBC_SONAMES='libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1
libresolv.so.2 libutil.so.1 ld-linux-aarch64.so.1 libgcc_s.so.1'

needed() {
    docker run --rm -v "$(cd "$LIBDIR" && pwd)":/libs "$IMAGE" sh -c \
      'for f in /libs/*.so*; do aarch64-linux-gnu-readelf -d "$f" 2>/dev/null; done' \
      | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p' | sort -u
}

pass=0
while [ "$pass" -lt 10 ]; do
    pass=$((pass + 1))
    missing=""
    for so in $(needed); do
        [ -e "$LIBDIR/$so" ] && continue
        skip=0
        for g in $GLIBC_SONAMES; do [ "$so" = "$g" ] && skip=1; done
        [ "$skip" = 1 ] || missing="$missing $so"
    done
    [ -z "$missing" ] && break
    for so in $missing; do
        found=""
        for d in /usr/trimui/lib /usr/lib /lib; do
            if adb shell "test -e $d/$so" 2>/dev/null; then found="$d/$so"; break; fi
        done
        [ -n "$found" ] || fail "$so needed but not found on device"
        say "pulling dependency $found"
        pull_lib "$found" || fail "could not pull $found"
    done
done
[ "$pass" -lt 10 ] || fail "dependency closure did not converge"

# --- headers, from upstream ------------------------------------------------

TARBALL="$CACHE/SDL2-${SDL_VERSION}.tar.gz"
if [ ! -e "$TARBALL" ] || ! echo "$SDL_SHA256  $TARBALL" | shasum -a 256 -c - >/dev/null 2>&1; then
    say "downloading SDL2 $SDL_VERSION headers from upstream"
    curl -sSLf -o "$TARBALL" "$SDL_URL"
fi
echo "$SDL_SHA256  $TARBALL" | shasum -a 256 -c - >/dev/null \
    || fail "SDL2 tarball hash mismatch"

# Only include/. The release tarball's SDL_config.h falls back to
# SDL_config_minimal.h on Linux; for an API consumer that is sound - sized
# types come from stdint.h and no public struct layout depends on it. The
# generated-config path matters only when building SDL itself.
tar xzf "$TARBALL" -C "$CACHE" "SDL2-${SDL_VERSION}/include"
cp "$CACHE/SDL2-${SDL_VERSION}/include/"*.h "$INCDIR/"

# --- provenance ------------------------------------------------------------

{
    echo "generated: $(date +%Y-%m-%d)"
    echo "device:    $(adb get-serialno)"
    echo "kernel:    $(adb shell uname -r | tr -d '\r')"
    echo "glibc:     $(adb shell readlink /lib/libc.so.6 | tr -d '\r')"
    echo "sdl2:      $(grep -aom1 'SDL-[0-9][0-9.]*-[a-z-]*' "$LIBDIR/libSDL2-2.0.so.0" || echo unknown)"
    echo "headers:   SDL2 ${SDL_VERSION} upstream, sha256 ${SDL_SHA256}"
} > "$SYSROOT/PROVENANCE"

say "done:"
ls -la "$LIBDIR"
cat "$SYSROOT/PROVENANCE"
