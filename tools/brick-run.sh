#!/bin/sh
# Runs a Diatom build on the Brick, borrowing the display from the device UI
# and giving it back afterwards.
#
#   tools/brick-run.sh                          stub core, integer mode
#   tools/brick-run.sh --display aspect         a specific starting mode
#   tools/brick-run.sh --core X.so --rom game   anything already staged
#   tools/brick-run.sh --exec '<shell>'         any command, same guard
#   DIATOM_GAIN=15 tools/brick-run.sh ...       volume; INVERTED, lower is LOUDER (0-63)
#
# It REFUSES to start if anything is already presenting. Two presenters wedge
# the framebuffer in-kernel and cost a power cycle - measured twice now.
#
# Use --exec for anything that runs Diatom more than once, or runs it in a
# loop. Hand-writing an `adb shell` that skips the freeze is how the display
# engine gets wedged, and recovering from that needs a reboot.
#
# Everything after the script name is passed through to diatom, so --core,
# --rom, --display, --frames and --shot all work. With no --core, the staged
# NES core and ROM are used.
#
# Why a script and not three adb commands: taking the display on this device is
# a sequence with two traps in it, both hit for real (see ADR-0013 and the
# 2026-08-24 session log).
#
#   1. adbd runs UNDER the PlayOS launch chain. Sweeping the process group
#      kills ADB itself and needs a physical power cycle to recover.
#   2. launch.sh is a supervisor: killing the UI processes alone just makes it
#      respawn them, and two processes presenting at once wedges the PowerVR
#      firmware in-kernel, which also needs a power cycle.
#
# So: STOP the supervisor, kill the UI, run, then CONT the supervisor and let
# it put the UI back. The trap matters more than the happy path.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
STAGE=/mnt/SDCARD/diatom
BIN=$ROOT/build/brick/diatom

[ -x "$BIN" ] || { echo "brick-run: no $BIN; run tools/brick-make.sh" >&2; exit 1; }
adb get-state >/dev/null 2>&1 || { echo "brick-run: no device over adb" >&2; exit 1; }

adb shell "mkdir -p $STAGE" >/dev/null
adb push "$BIN" "$STAGE/diatom" >/dev/null
adb push "$ROOT/tools/brick-device-run.sh" "$STAGE/run.sh" >/dev/null
adb shell "chmod +x $STAGE/diatom $STAGE/run.sh" >/dev/null

# --exec ships the command as a FILE rather than a string: it would otherwise
# have to survive this shell, adb's argument handling, and the device shell,
# and the quoting does not make it.
if [ "${1:-}" = "--exec" ]; then
    shift
    tmp=$(mktemp)
    printf '%s\n' "$*" > "$tmp"
    adb push "$tmp" "$STAGE/exec.sh" >/dev/null
    rm -f "$tmp"
    exec adb shell "DIATOM_GAIN='${DIATOM_GAIN:-}' $STAGE/run.sh --exec-file $STAGE/exec.sh"
fi

# The on-device half does the freeze, run and restore, so an interrupted adb
# connection cannot leave the supervisor stopped.
# Quote every argument. `$*` flattened them into a raw shell string that the
# device shell then re-parsed, so any ROM whose name contains parentheses -
# which is nearly every No-Intro and Redump dump - died with
# `syntax error: unexpected "("` before anything ran. Latent until 2026-08-25
# because every prior test happened to go through --exec, which ships a file.
q=""
for a in "$@"; do
    q="$q '$(printf '%s' "$a" | sed "s/'/'\\\\''/g")'"
done
exec adb shell "DIATOM_GAIN='${DIATOM_GAIN:-}' $STAGE/run.sh$q"
