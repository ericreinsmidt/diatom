#!/bin/sh
# Regenerate docs/reference/core-facts.md by MEASURING, not by remembering.
#
#   tools/corefacts.sh            measure and rewrite the facts file
#   tools/corefacts.sh --check    measure and fail if the file is out of date
#
# Why this exists
# ---------------
# On 2026-08-25 an audit found that four of the six rows in ADR-0015's aspect
# table described cores the project had since rejected. PicoDrive reports
# Genesis at aspect 1.3333; Genesis Plus GX, which replaced it, reports 1.5238.
# Snes9x 1.63 reports 1.3333 and max 604x478; snes9x2010, which replaced it,
# reports 1.5842 for PAL and max 1024x478. Nothing could detect that, because
# the table was keyed on the SYSTEM. A system does not report an aspect - a
# specific core build does, and the number had been separated from the binary
# that produced it.
#
# CORES.md already said the intent out loud: "this file exists so the
# measurements in docs/ name the exact bytes that produced them." Saying it was
# not enough. This measures it.
#
# The same audit then made the same mistake: three of the five cores it measured
# on the device were not the pinned binaries at all, because the device staging
# directory had drifted from CORES.md. So the first thing here is not the
# measurement, it is verifying the bytes - both on the host and on the device
# after the push. A measurement of the wrong binary is worse than no
# measurement, because it looks like evidence.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/docs/reference/core-facts.md
STAGE=/mnt/SDCARD/diatom/facts
ROMS=${DIATOM_ROMS:-/mnt/SDCARD/diatom/m}
CHECK=0
[ "${1:-}" = "--check" ] && CHECK=1

# system | core | rom | firmware
# One row per distinct thing a core can report. PAL and NTSC are separate rows
# because fps AND aspect differ between them - a single "SNES" row cannot be
# right, which is half of what went wrong in ADR-0015.
MATRIX="NES (PAL)|fceumm|Probotector (Europe).nes|
NES (NTSC)|fceumm|Final Fantasy (USA).nes|
SNES (PAL)|snes9x2010|Parodius (Europe).sfc|
SNES (NTSC)|snes9x2010|Star Fox (USA) (Rev 2).sfc|
Game Boy|mgba|Tetris (World) (Rev 1).gb|
GBA|mgba|Golden Sun (USA, Europe).gba|
Genesis|genesis_plus_gx|Phantasy Star IV (USA).md|
Master System|genesis_plus_gx|Sonic The Hedgehog (USA, Europe, Brazil).sms|
Game Gear|genesis_plus_gx|Sonic The Hedgehog (Japan, Europe, Brazil) (En) (Rev 1).gg|
PC Engine|mednafen_pce_fast|Drop Off (U).pce|"

# ---------------------------------------------------------------- host side

# Every core must match CORES.md before anything is pushed anywhere. This is
# the cheap half and it runs first, because it needs no device.
pin_of() { grep -oE "\`$1\` \| [0-9]+ \| \`[a-f0-9]{64}\`" "$ROOT/CORES.md" | grep -oE '[a-f0-9]{64}'; }

cores=$(printf '%s\n' "$MATRIX" | cut -d'|' -f2 | sort -u)
bad=0
for c in $cores; do
    f=$ROOT/cores/${c}_libretro.so
    [ -f "$f" ] || { echo "corefacts: missing $f - run tools/fetch-cores.sh" >&2; bad=1; continue; }
    have=$(shasum -a 256 "$f" | cut -d' ' -f1)
    want=$(pin_of "$c")
    [ -n "$want" ] || { echo "corefacts: $c is not pinned in CORES.md" >&2; bad=1; continue; }
    [ "$have" = "$want" ] || {
        echo "corefacts: $c does not match CORES.md" >&2
        echo "    have $have" >&2
        echo "    want $want" >&2
        bad=1
    }
done
[ "$bad" = 0 ] || { echo "corefacts: refusing to measure unpinned binaries" >&2; exit 1; }

adb get-state >/dev/null 2>&1 || { echo "corefacts: no device over adb" >&2; exit 2; }

# -------------------------------------------------------------- device side

adb shell "mkdir -p $STAGE" >/dev/null
for c in $cores; do
    adb push "$ROOT/cores/${c}_libretro.so" "$STAGE/" >/dev/null
done
adb push "$ROOT/build/brick/diatom" "$STAGE/diatom" >/dev/null
adb shell "chmod +x $STAGE/diatom" >/dev/null

# Verify again ON THE DEVICE. A push can truncate, and the staging directory is
# exactly where the drift that started all this actually lived.
for c in $cores; do
    got=$(adb shell "sha256sum $STAGE/${c}_libretro.so" 2>/dev/null | cut -d' ' -f1 | tr -d '\r')
    [ "$got" = "$(pin_of "$c")" ] || {
        echo "corefacts: $c on the device does not match CORES.md after push" >&2
        exit 1
    }
done

script=$(mktemp)
{
    echo "cd $STAGE"
    echo 'export LD_LIBRARY_PATH=/usr/trimui/lib'
    printf '%s\n' "$MATRIX" | while IFS='|' read -r sys core rom fw; do
        [ -n "$sys" ] || continue
        [ -n "$fw" ] && fwarg="--firmware \"$fw\"" || fwarg=""
        echo "printf '%s\\t' \"$sys\" \"$core\""
        echo "./diatom --core \"$STAGE/${core}_libretro.so\" --rom \"$ROMS/$rom\" \\"
        # 30 frames was cutting it fine: Herzog Zwei settles at frame 27, so a
        # slightly slower boot would have measured the boot mode and looked
        # completely stable doing it. 180 matches SETTLE_FRAMES in main.c, which
        # is the window Diatom will actually relock within - measuring past that
        # would record a geometry the frontend would never adopt.
        echo "  --system \"$ROMS\" $fwarg --frames 180 2>&1 \\"
        # tail, not head. The FIRST line is what the core reports at load, and
        # for Genesis that is 29 frames of boot mode: 256x192 at 1.5238, where
        # the game actually runs 320x224 at 1.3061. Diatom reprints the line in
        # the same format once the geometry settles, so the last one is the mode
        # that holds. Taking the first is what put a boot artefact in this file
        # and, from there, into ADR-0018's display table.
        echo "  | grep -E '^diatom: [0-9]+x[0-9]+' | tail -1"
    done
} > "$script"

raw=$("$ROOT/tools/brick-run.sh" --exec "$(cat "$script")" 2>/dev/null | grep -E '	[a-z0-9_]+	diatom:')
rm -f "$script"

# ------------------------------------------------------------------ emit

tmp=$(mktemp)
{
    echo "# Core facts - generated, do not edit"
    echo
    echo "Regenerate with \`tools/corefacts.sh\`. Verify with \`tools/corefacts.sh --check\`."
    echo
    echo "**Every number here was measured, on the device, from a binary whose"
    echo "sha256 matched [CORES.md](../../CORES.md) both before and after the push.**"
    echo "Cite this file rather than restating its numbers - a figure copied into"
    echo "prose cannot be re-checked, which is how ADR-0015 came to describe four"
    echo "cores the project no longer runs."
    echo
    echo "Rows are per *content*, not per system: PAL and NTSC differ in both fps"
    echo "and aspect, so one row per system cannot be correct."
    echo
    echo "| System | Core | sha256 | Base | Max | Aspect | fps | Rate |"
    echo "|---|---|---|---|---|---|---|---|"
    printf '%s\n' "$raw" | while IFS='	' read -r sys core line; do
        geom=$(echo "$line"  | sed -n 's/.*: \([0-9]*x[0-9]*\) .*/\1/p')
        max=$(echo "$line"   | sed -n 's/.*(max \([0-9]*x[0-9]*\)).*/\1/p')
        asp=$(echo "$line"   | sed -n 's/.*aspect \([0-9.]*\).*/\1/p')
        fps=$(echo "$line"   | sed -n 's/.*, \([0-9.]*\) fps.*/\1/p')
        rate=$(echo "$line"  | sed -n 's/.*fps, \([0-9]*\) Hz.*/\1/p')
        sha=$(pin_of "$core" | cut -c1-12)
        echo "| $sys | \`$core\` | \`$sha\` | $geom | $max | $asp | $fps | $rate |"
    done
    echo
    echo "## Pinned set at time of measurement"
    echo
    echo "A change here invalidates every row above, which is the whole point:"
    echo "\`make check\` compares this list against CORES.md without needing a"
    echo "device, so swapping a core fails immediately rather than silently"
    echo "aging every measurement that cited it."
    echo
    for c in $cores; do echo "- \`$c\` \`$(pin_of "$c")\`"; done
} > "$tmp"

if [ "$CHECK" = 1 ]; then
    if diff -u "$OUT" "$tmp" >/dev/null 2>&1; then
        echo "ok: core facts match the device"
        rm -f "$tmp"
    else
        echo "FAIL: measured facts differ from $OUT" >&2
        diff -u "$OUT" "$tmp" >&2 || true
        rm -f "$tmp"
        exit 1
    fi
else
    mv "$tmp" "$OUT"
    echo "wrote $OUT"
fi
