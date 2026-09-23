# Cross-toolchain for the TrimUI Brick (TG3040) - ADR-0012.
#
# Nothing in here comes from NextUI, MinUI, LoveRetro or TrimUI's SDK. A stock
# Debian cross-compiler is the whole toolchain; the platform's SDL2 headers and
# libraries live in the sysroot (tools/fetch-brick-sysroot.sh), not in this
# image, so the image never needs rebuilding when the device libraries change.
#
# Base is bullseye because of glibc, not preference: the image's glibc is the
# FLOOR of what the output binary demands, and it must stay at or below the
# device's 2.33. Bullseye's 2.31 clears that; bookworm's 2.36 would not.
# Verified against firmware 1.1.1, 2026-08-24.
#
# Pinned by digest, never :latest. An unpinned upstream is the drift failure
# this project exists to avoid. This digest is the multi-arch manifest list;
# on an arm64 host the image runs natively, no emulation involved.
#
#   docker build -f tools/brick-toolchain.Dockerfile -t diatom-brick-toolchain tools
#
# python3 is for tools/gen_env_names.py, which `make tools` runs.
FROM debian:bullseye-slim@sha256:f313b4bd62667092a59b3a664d7d3ab8b5e65f41675f48e81455a15dc5abe792

# apt reads from a fixed snapshot, not the live mirror. Bullseye is past end
# of life: by 2026-09-23 its security updates had left deb.debian.org's pool
# while the index still named them, so a plain apt-get install failed on 404s.
# A snapshot also gets the same packages on every build, for the same reason
# the base is pinned by digest. Valid-Until is off because a snapshot's Release
# file is past its own expiry by design; the signatures are still checked.
ARG DEBIAN_SNAPSHOT=20260901T000000Z
RUN printf '%s\n' \
        "deb http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} bullseye main" \
        "deb http://snapshot.debian.org/archive/debian-security/${DEBIAN_SNAPSHOT} bullseye-security main" \
        "deb http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} bullseye-updates main" \
        > /etc/apt/sources.list

RUN apt-get -o Acquire::Check-Valid-Until=false update && apt-get install -y --no-install-recommends \
        gcc-aarch64-linux-gnu \
        libc6-dev-arm64-cross \
        binutils-aarch64-linux-gnu \
        make \
        python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
