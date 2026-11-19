#!/usr/bin/env bash
# prep/ (.r3d -> MP4, as on the console) on this computer: one slice, two slices (the New 3DS app's
# mp4_threads default) and two slices stopped and resumed (each run gets past a resume point: they
# come every PREP_GOP frames). Each MP4 must decode whole, with as many video frames as the others and
# its sound. Needs ffmpeg/ffprobe and node (the synthetic recording).
# Usage: tests/prep/run.sh [recording.r3d]   (default: a 12 s synthetic one, both screens with sound)
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$REPO/build/prep-test"
mkdir -p "$OUT"
make -s -C "$REPO/tests/prep"
IN="${1:-$OUT/synthetic.r3d}"
[ -n "${1:-}" ] || node "$REPO/tests/web/make-r3d.mjs" "$IN" --secs 12 --fps 30 >/dev/null

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
resumed=$(check resumed --threads 2 --interrupt 150 | tee /dev/stderr | tail -1)
[ "$one" = "$two" ] && [ "$two" = "$resumed" ] || { echo "frame counts differ: $one / $two / $resumed"; exit 1; }
echo "prep: ok"
