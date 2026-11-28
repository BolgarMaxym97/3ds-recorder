#!/usr/bin/env bash
# prep/ (.r3d -> MP4, as on the console) on this computer: one slice, two slices (the New 3DS app's
# mp4_threads default) and two slices stopped and resumed (each run gets past a resume point: they
# come every PREP_CKPT frames). Each MP4 must decode whole, with as many video frames as the others and
# its sound. Needs ffmpeg/ffprobe and node (the synthetic recording).
# Usage: tests/prep/run.sh [recording.r3d]   (default: a 30 s synthetic one, both screens with sound)
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$REPO/build/prep-test"
mkdir -p "$OUT"
make -s -C "$REPO/tests/prep"
IN="${1:-$OUT/synthetic.r3d}"
[ -n "${1:-}" ] || node "$REPO/tests/web/make-r3d.mjs" "$IN" --secs 30 --fps 30 >/dev/null

frames() { ffprobe -v error -select_streams v:0 -count_frames -show_entries stream=nb_read_frames -of csv=p=0 "$1"; }
check() {   # name, host_prep options
    local name="$1"; shift
    rm -f "$OUT/$name".*
    "$REPO/build/host_prep" "$IN" "$OUT/$name" "$@" 2>"$OUT/$name.log" || { cat "$OUT/$name.log"; exit 1; }
    ffmpeg -v error -i "$OUT/$name.mp4" -f null - 2>"$OUT/$name.decode" || true
    [ ! -s "$OUT/$name.decode" ] || { echo "$name: decode errors"; cat "$OUT/$name.decode"; exit 1; }
    local f a
    f=$(frames "$OUT/$name.mp4")
    a=$(ffprobe -v error -select_streams a -show_entries stream=codec_name -of csv=p=0 "$OUT/$name.mp4")
    echo "$name: $f video frames, audio ${a:-none}"
    echo "$f"
}

one=$(check one --threads 1 | tee /dev/stderr | tail -1)
two=$(check two --threads 2 | tee /dev/stderr | tail -1)
resumed=$(check resumed --threads 2 --interrupt 600 | tee /dev/stderr | tail -1)
[ "$one" = "$two" ] && [ "$two" = "$resumed" ] || { echo "frame counts differ: $one / $two / $resumed"; exit 1; }

# A picture that stays (2 s to 4 s) is encoded once more as a sharp key frame, repeats are not encoded;
# the buttons drawn when asked (SELECT held at 1.25 s: its pill in the left strip, x 20, y 410, is white)
STILL="$OUT/held-src.r3d"   # (check removes $OUT/<name>.*)
node "$REPO/tests/web/make-r3d.mjs" "$STILL" --secs 30 --fps 30 --still --inputs --unknown >/dev/null
IN="$STILL"
plain=$(check still --threads 1 | tail -1)
keys=$(check keys --threads 1 --inputs 1 | tail -1)
[ "$plain" -lt 870 ] || { echo "still: $plain frames, the held picture was encoded again and again"; exit 1; }
ktime=$(ffprobe -v error -select_streams v:0 -show_entries frame=pts_time,pict_type -of csv=p=0 "$OUT/still.mp4" |
    awk -F, '$1 > 1.95 && $1 < 2.1 && $2 == "I" { print $1 }' | head -1)
[ -n "$ktime" ] || { echo "still: the held picture (2 s) is not a key frame"; exit 1; }
px() { ffmpeg -v error -ss "$2" -i "$1" -frames:v 1 -f rawvideo -pix_fmt rgb24 - | python3 -c "
import sys
d = sys.stdin.buffer.read(); i = (410 * 400 + 20) * 3
print(d[i], d[i + 1], d[i + 2])"; }
read -r r g b <<< "$(px "$OUT/keys.mp4" 1.3)"
[ "$r" -gt 170 ] && [ "$g" -gt 170 ] && [ "$b" -gt 170 ] || { echo "keys: SELECT not drawn ($r $g $b)"; exit 1; }
read -r r g b <<< "$(px "$OUT/still.mp4" 1.3)"
[ "$r" -lt 60 ] || { echo "still: buttons drawn without --inputs ($r $g $b)"; exit 1; }
! grep -q "SPS/PPS changed" "$OUT/still.log" "$OUT/keys.log" || { echo "the held key frame changed the SPS/PPS"; exit 1; }
read -r r g b <<< "$(px "$OUT/still.mp4" 0.5)"   # the black strip beside the bottom screen stays black
[ "$r" -lt 24 ] && [ "$g" -lt 24 ] || { echo "still: the picture before the held one decodes wrong ($r $g $b)"; exit 1; }
echo "still: $plain frames (key frame at $ktime s), buttons: SELECT drawn"
resumedKeys=$(check keysresumed --threads 2 --inputs 1 --interrupt 600 | tail -1)
[ "$resumedKeys" = "$keys" ] || { echo "buttons, resumed: $resumedKeys frames, $keys without stopping"; exit 1; }
echo "prep: ok"
