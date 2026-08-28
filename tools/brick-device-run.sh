#!/bin/sh
# Runs Diatom on the Brick with exclusive use of the display, then puts the
# device UI back. Staged at /mnt/SDCARD/diatom/run.sh; usually invoked through
# tools/brick-run.sh on the development machine.
#
# The restore is in a trap because the failure mode is expensive: leaving the
# TortOS supervisor stopped looks like a bricked device, and leaving two
# processes presenting at once wedges the GPU firmware until a power cycle.
HERE=/mnt/SDCARD/diatom
CORE=$HERE/fceumm_libretro.so
ROM="$HERE/Contra (USA).nes"

# Both names, deliberately. The card is mid-rename from PlayOS to TortOS, and
# an empty SUP is not a harmless miss: the supervisor is never frozen, it
# respawns the UI underneath Diatom, and two presenters wedge the display
# engine until a power cycle. Matching a name that is not there costs nothing;
# missing the one that is costs a reboot. Drop PlayOS once no card runs it.
SUP=$(ps | grep -E 'TortOS/launch\.sh|PlayOS/launch\.sh' | grep -v grep | awk '{print $1}')

# Refuse to start if something is already presenting.
#
# The guard above protects against the TortOS UI and did so correctly. It did
# nothing about a PREVIOUS Diatom, and on 2026-08-25 a second instance launched
# on top of a live one. Two presenters is the one thing ADR-0013 says never to
# do: it left tortos.elf unkillable in fb_open holding the kernel framebuffer
# lock, and cost a power cycle.
#
# The cleanup that failed was `pkill -f ...`, and the reason is worth keeping:
# **busybox here has no pkill at all.** It exited 127, the exit status was not
# checked, and "no such command" looked exactly like "nothing to kill". Use
# `ps | grep | xargs kill`, and check STATE rather than trusting any kill.
BUSY=$(ps | grep -E '[/ ]diatom( |$)|/diatom --' | grep -v grep | grep -v "$$" | awk '{print $1}')
if [ -n "$BUSY" ]; then
    echo "brick-run: REFUSING - something is already presenting:" >&2
    ps | grep -E '[/ ]diatom( |$)|/diatom --' | grep -v grep >&2
    echo "brick-run: kill it first; two presenters wedge the display (ADR-0013)" >&2
    exit 3
fi

# Output gain, opt-in via DIATOM_GAIN. TortOS resets the mixer when its UI
# resumes, so setting this from the host before the freeze is always pointless -
# it has to happen here, inside it.
#
# Measured 2026-08-25: the chain is four controls and only one of them was
# actually attenuating. `digital volume` sat at 37 of 63, which is -31.3 dB,
# while `DAC volume` and `Headphone` both read as set. Reading one control at a
# time hides that; the whole chain has to be set together or not at all.
# DIATOM_GAIN drives `digital volume`, and that control is **INVERTED**:
# it is an ATTENUATION register, so a LOWER number is LOUDER.
#
#   0  = loudest      15 = a normal listening level      63 = silent
#
# Proven by diffing tinymix across a volume-up press in TortOS on 2026-08-25:
# the value went 37 -> 15 when the user turned it UP. The driver's own metadata
# claims `dBscale-min=-74.24dB, step=+1.16dB`, i.e. that higher is louder. That
# metadata is wrong, and trusting it cost two hours: every "louder" setting made
# it quieter, and 63 - set as "maximum" - is silence.
#
# `Headphone Volume` stays 0 and is not a speaker level: raising it routes to
# the headphone JACK and mutes the speakers. TortOS zeroes it deliberately.
if [ -n "${DIATOM_GAIN:-}" ]; then
    amixer sset 'Headphone' 0            >/dev/null 2>&1
    amixer sset 'digital volume' "${DIATOM_GAIN}" >/dev/null 2>&1
    amixer sset 'DAC volume' 200         >/dev/null 2>&1
    amixer sset 'Soft Volume Master' 255 >/dev/null 2>&1
fi

restore() {
    [ -n "$SUP" ] && kill -CONT "$SUP" 2>/dev/null
    exit "${1:-0}"
}
trap 'restore 130' INT TERM HUP

# Freeze the supervisor FIRST, or it respawns the UI underneath us.
[ -n "$SUP" ] && kill -STOP "$SUP" 2>/dev/null
killall -9 tortos.elf playos.elf minarch.elf 2>/dev/null   # see SUP above
sleep 1

# --exec runs an arbitrary command inside the same freeze, instead of diatom.
# It exists because the alternative is hand-writing an unguarded `adb shell`
# for any multi-step experiment - which is exactly how the display got wedged
# on 2026-08-24, costing a reboot. Anything that presents must come through
# here.
if [ "${1:-}" = "--exec-file" ]; then
    shift
    cd "$HERE" || restore 1
    sh "$1"
    restore $?
fi

case "$*" in
    *--core*) set -- "$@" ;;
    *)        set -- --core "$CORE" --rom "$ROM" "$@" ;;
esac

echo "diatom: SELECT+R1 / SELECT+L1  next / previous display mode"
echo "diatom: SELECT+A             toggle nearest <-> sharp filter"
echo "diatom: MENU                 exit and restore the device UI"

cd "$HERE" || restore 1
LD_LIBRARY_PATH=/usr/trimui/lib ./diatom "$@"
RC=$?

restore "$RC"
