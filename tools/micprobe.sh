#!/bin/sh
# Did sound actually come out of the speaker? Answered as a number.
#
#   tools/micprobe.sh <core.so> <rom> [seconds]
#   tools/micprobe.sh --tone                     known-good reference
#
# The Brick has a capture device on the same card as playback, and its mic
# hears its own speaker. So "is there audio" stops being a question only a
# human in the room can answer and becomes an RMS figure:
#
#   silence          rms   ~25
#   Probotector      rms  ~695
#   full-scale sine  rms ~1565
#
# Written 2026-08-25, after an evening spent asking a human to listen and
# report back. Every report they gave was accurate and consistent - "very
# quiet", "barely audible", "still quiet at max" - and every one described a
# LEVEL. Without a baseline to compare against, none of them could be acted on,
# so they got treated as symptoms of a fault instead. There was no fault: the
# audio path worked the whole time, and `digital volume` is inverted so
# "maximum" was silence.
#
# The lesson is not "be more careful listening". It is that a question phrased
# as "does this sound right" has a number behind it, and the number was ten
# minutes of work away using tools already on the device.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SECS=${3:-4}
STAGE=/mnt/SDCARD/diatom/facts
GAIN=${DIATOM_GAIN:-15}

adb get-state >/dev/null 2>&1 || { echo "micprobe: no device over adb" >&2; exit 2; }

if [ "${1:-}" = "--tone" ]; then
    python3 - "$ROOT" <<'PY'
import struct, math, sys
sr, n = 48000, 48000 * 3
d = b''.join(struct.pack('<hh', int(28000*math.sin(2*math.pi*440*i/sr)),
                                int(28000*math.sin(2*math.pi*440*i/sr))) for i in range(n))
hdr = (b'RIFF' + struct.pack('<I', 36+len(d)) + b'WAVEfmt ' +
       struct.pack('<IHHIIHH', 16, 1, 2, sr, sr*4, 4, 16) + b'data' + struct.pack('<I', len(d)))
open('/tmp/micprobe-tone.wav','wb').write(hdr+d)
PY
    adb push /tmp/micprobe-tone.wav /tmp/tone.wav >/dev/null
    BODY="aplay -D hw:0,0 /tmp/tone.wav >/dev/null 2>&1"
    LABEL="440Hz tone at -1.4dBFS"
else
    [ $# -ge 2 ] || { echo "usage: tools/micprobe.sh <core.so> <rom> [secs]" >&2; exit 1; }
    BODY="cd $STAGE; export LD_LIBRARY_PATH=/usr/trimui/lib; ./diatom --core \"$1\" --rom \"$2\" --system /mnt/SDCARD/diatom/m --frames 900 >/tmp/mp.log 2>&1"
    LABEL="$(basename "$2")"
fi

cat > /tmp/micprobe-dev.sh <<EOF
tinymix set 'Headphone Volume' 0 >/dev/null 2>&1
tinymix set 'digital volume' $GAIN >/dev/null 2>&1
arecord -D hw:0,0 -f S16_LE -r 48000 -c 2 -d 2 /tmp/mp_base.wav >/dev/null 2>&1
( $BODY ) &
P=\$!
sleep 5
arecord -D hw:0,0 -f S16_LE -r 48000 -c 2 -d $SECS /tmp/mp_run.wav >/dev/null 2>&1
wait \$P 2>/dev/null || true
EOF

"$ROOT/tools/brick-run.sh" --exec "$(cat /tmp/micprobe-dev.sh)" >/dev/null 2>&1 || true
adb pull /tmp/mp_base.wav /tmp/ >/dev/null 2>&1
adb pull /tmp/mp_run.wav  /tmp/ >/dev/null 2>&1

python3 - "$LABEL" "$GAIN" <<'PY'
import wave, struct, math, sys
def stat(p):
    w = wave.open(p); d = w.readframes(w.getnframes())
    s = struct.unpack('<%dh' % (len(d)//2), d)
    return math.sqrt(sum(x*x for x in s)/len(s)), max(abs(x) for x in s)
try:
    b_rms, b_pk = stat('/tmp/mp_base.wav')
    r_rms, r_pk = stat('/tmp/mp_run.wav')
except Exception as e:
    print("micprobe: no capture (%s)" % e); raise SystemExit(1)
print("  %-32s digital volume %s" % (sys.argv[1], sys.argv[2]))
print("  baseline (room)   rms %7.1f  peak %6d" % (b_rms, b_pk))
print("  while playing     rms %7.1f  peak %6d" % (r_rms, r_pk))
print("  ->  %s (%.1fx over baseline)" %
      ("AUDIBLE" if r_rms > b_rms * 4 else "** NO AUDIO **", r_rms / max(b_rms, 1)))
PY
