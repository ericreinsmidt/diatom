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
exec docker run --rm -u "$(id -u):$(id -g)" \
    -v "$ROOT":/work -w /work "$IMAGE" make PORT=brick "$@"
