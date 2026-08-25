#!/bin/sh
# Runs make for the Brick inside the toolchain container - ADR-0012.
#
#   tools/brick-make.sh            builds the frontend
#   tools/brick-make.sh stub       cross-builds the stub core
#   tools/brick-make.sh tools      cross-builds the instruments
#
# The container never sees the device; the sysroot is fetched separately by
# tools/fetch-brick-sysroot.sh, on the host, where adb is.
set -eu

IMAGE=diatom-brick-toolchain
ROOT=$(cd "$(dirname "$0")/.." && pwd)

docker image inspect "$IMAGE" >/dev/null 2>&1 || {
    printf 'brick-make: toolchain image missing; build it first:\n' >&2
    printf '  docker build -f tools/brick-toolchain.Dockerfile -t %s tools\n' "$IMAGE" >&2
    exit 1
}

# Run as the invoking user so build/ is not root-owned on Linux hosts. The
# user has no passwd entry inside the container, which gcc and make tolerate.
docker run --rm -u "$(id -u):$(id -g)" \
    -v "$ROOT":/work -w /work "$IMAGE" make PORT=brick "$@"
rc=$?

# Staleness check, because make has silently skipped rebuilds here twice.
#
# Docker on macOS can show the container a stale mtime for a file the host just
# wrote, so make concludes a target is current when its source is not. The
# failure is invisible: the build reports success, the push reports success, and
# the test exercises the previous binary. Both times it read as a bug in the
# feature under test - once as SAVE silently doing nothing, once as a fix that
# appeared not to work.
#
# So: after building, refuse to be quiet about an output older than ITS OWN
# source. Comparing against the newest source anywhere flags every target
# whenever one file changes, which is noise - and a check people learn to ignore
# is worse than no check.
newest_of() { ls -t "$@" 2>/dev/null | head -1; }
stale=0
check() {   # check <output> <source>...
    out=$1; shift
    [ -f "$out" ] || return 0
    src=$(newest_of "$@")
    [ -n "$src" ] || return 0
    if [ "$src" -nt "$out" ]; then
        echo "brick-make: STALE - $(basename "$out") is older than $(basename "$src")" >&2
        stale=1
    fi
}

check "$ROOT/build/brick/diatom" \
      "$ROOT"/src/*.c "$ROOT"/src/*.h "$ROOT"/include/*.h "$ROOT"/port/brick.c
for out in "$ROOT"/build/brick/tools/*; do
    [ -f "$out" ] || continue
    check "$out" "$ROOT/tools/$(basename "$out").c"
done

[ "$stale" = 1 ] && \
    echo "brick-make: the container can see a stale mtime; re-run, or 'make -B'" >&2

exit $rc
