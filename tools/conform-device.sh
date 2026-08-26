#!/bin/sh
# Headless conformance, device half: the space claims, on the hardware they are
# about.
#
# `test/conform.py` covers determinism on the desktop, where it is fast and
# needs no device. Two things it cannot cover live here:
#
#   allocations   `-Wl,--wrap=malloc` is a GNU ld feature and macOS has no
#                 equivalent, so the count only exists on a Linux build.
#   RSS           a desktop figure is mostly SDL and the host window system.
#                 §11's budget is about a 975 MB handheld, so it has to be
#                 asserted on one.
#
# Both are differential: run N and 2N frames and compare. Start-up costs are
# identical in both, so anything per-frame is the difference. Nothing is armed
# or hooked inside Diatom, which is why no part of this ships.
#
#   tools/conform-device.sh
#
# Takes the display for about 25 seconds and puts it back. Silent: the gain is
# pinned to the mixer's minimum, which is inaudible across a room.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
H=/mnt/SDCARD/diatom

# Measured 2026-08-26 on a TrimUI Brick: 8.2-8.4 MB peak with FCEUmm loaded and
# running, flat across 300, 600 and 1200 frames. `tools/rssprobe.c` puts all six
# cores mapped with one running at 15.0 MB, so this one ceiling covers the
# heaviest configuration the project ships as well as the case tested here.
# ADR-0006 names 250 MB, which §10 records as badly calibrated at 16x; this is
# the number that would actually notice.
# Overridable so the assertion itself can be tested: a check that has never
# been seen to fail is a check nobody knows is wired up.
CEILING_KB=${DIATOM_RSS_CEILING_KB:-$((24 * 1024))}
N=300

[ -x "$ROOT/build/brick/diatom-conform" ] || {
    echo "conform-device: no wrapped build; run tools/brick-make.sh conform" >&2
    exit 1
}
adb push "$ROOT/build/brick/diatom-conform" "$H/diatom-conform" >/dev/null
adb shell "chmod +x $H/diatom-conform"

# Hash both ends rather than trusting the push. A stale device binary has
# silently passed for a working one three times on this project.
want=$(md5 -q "$ROOT/build/brick/diatom-conform" 2>/dev/null \
       || md5sum "$ROOT/build/brick/diatom-conform" | cut -d' ' -f1)
got=$(adb shell "md5sum $H/diatom-conform" | cut -d' ' -f1 | tr -d '\r')
[ "$want" = "$got" ] || { echo "conform-device: pushed binary does not match" >&2; exit 1; }

out=$(DIATOM_GAIN=63 "$ROOT/tools/brick-run.sh" --exec "H=$H
export LD_LIBRARY_PATH=/usr/trimui/lib
for N in $N $((N * 2)); do
  \$H/diatom-conform --core \$H/fceumm_libretro.so --rom \"\$H/Contra (USA).nes\" \\
      --frames \$N > /tmp/cf.log 2>&1 &
  P=\$!
  HWM=0
  while [ -d /proc/\$P ]; do
    V=\$(awk '/VmHWM/{print \$2}' /proc/\$P/status 2>/dev/null)
    [ -n \"\$V\" ] && HWM=\$V
    sleep 1
  done
  wait \$P
  echo \"RESULT frames=\$N rss=\$HWM \$(grep -o 'total=[0-9]*' /tmp/cf.log)\"
done" 2>&1)

echo "$out" | grep RESULT || { echo "$out" >&2; exit 1; }

a=$(echo "$out" | grep RESULT | sed -n 1p)
b=$(echo "$out" | grep RESULT | sed -n 2p)
na=$(echo "$a" | grep -o 'total=[0-9]*' | cut -d= -f2)
nb=$(echo "$b" | grep -o 'total=[0-9]*' | cut -d= -f2)
ra=$(echo "$a" | sed -n 's/.*rss=\([0-9]*\).*/\1/p')
rb=$(echo "$b" | sed -n 's/.*rss=\([0-9]*\).*/\1/p')

rc=0
if [ "$na" = "$nb" ] && [ -n "$na" ]; then
    echo "ok: Diatom's own allocations are $na at $N frames and at $((N * 2)) - nothing per frame"
else
    echo "FAIL: allocations $na at $N frames, $nb at $((N * 2)) - the frame loop allocates"
    rc=1
fi
for r in "$ra" "$rb"; do
    if [ -n "$r" ] && [ "$r" -lt "$CEILING_KB" ]; then :; else
        echo "FAIL: peak RSS ${r}kB is over the ${CEILING_KB}kB budget"
        rc=1
    fi
done
[ $rc -eq 0 ] && echo "ok: peak RSS ${ra}kB and ${rb}kB, both under the ${CEILING_KB}kB budget"
exit $rc
