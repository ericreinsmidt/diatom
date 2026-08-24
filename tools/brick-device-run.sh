#!/bin/sh
# Runs Diatom on the Brick with exclusive use of the display, then puts the
# device UI back. Staged at /mnt/SDCARD/diatom/run.sh; usually invoked through
# tools/brick-run.sh on the development machine.
#
# The restore is in a trap because the failure mode is expensive: leaving the
# PlayOS supervisor stopped looks like a bricked device, and leaving two
# processes presenting at once wedges the GPU firmware until a power cycle.
HERE=/mnt/SDCARD/diatom
CORE=$HERE/fceumm_libretro.so
ROM="$HERE/Contra (USA).nes"

SUP=$(ps | grep 'PlayOS/launch.sh' | grep -v grep | awk '{print $1}')

restore() {
    [ -n "$SUP" ] && kill -CONT "$SUP" 2>/dev/null
    exit "${1:-0}"
}
trap 'restore 130' INT TERM HUP

# Freeze the supervisor FIRST, or it respawns the UI underneath us.
[ -n "$SUP" ] && kill -STOP "$SUP" 2>/dev/null
killall -9 playos.elf minarch.elf 2>/dev/null
sleep 1

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
