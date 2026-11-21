#!/usr/bin/env bash
# End-to-end test of tools/r3d2video.py: a synthetic .r3d (make_r3d.c) through every preset and
# layout; checks sizes, codecs, audio, and that the screens land where they should (lossless).
# Needs cc, python3, ffmpeg, ffprobe.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$ROOT/build"
cc -std=c11 -O2 -Wall -Wextra -I"$ROOT/codec/include" -o "$ROOT/build/r3dconv" \
    "$ROOT/tools/r3dconv/r3dconv.c" "$ROOT/codec/source/codec.c" "$ROOT/codec/source/huff.c" -lm
cc -std=gnu11 -O2 -Wall -Wextra -I"$ROOT/codec/include" -o "$WORK/make_r3d" \
    "$ROOT/tests/pcconv/make_r3d.c" "$ROOT/codec/source/codec.c" -lm
"$WORK/make_r3d" "$WORK/rec.r3d"

conv() { python3 "$ROOT/tools/r3d2video.py" --out "$WORK/out" "$@" "$WORK/rec.r3d" > "$WORK/log" 2>&1 || { cat "$WORK/log"; exit 1; }; }
fail() { echo "FAIL: $*"; exit 1; }
probe() { ffprobe -v error -select_streams "$2" -show_entries stream="$3" -of csv=p=0 "$1"; }
expect() {   # file stream entries value
    local got; got="$(probe "$1" "$2" "$3")"
    [ "$got" = "$4" ] || fail "$(basename "$1") $3: got '$got', want '$4'"
    echo "ok  $(basename "$1")  $3=$got"
}

conv --preset youtube
expect "$WORK/out/rec_youtube.mp4" v:0 codec_name,width,height,pix_fmt "h264,1200,1440,yuv420p"
expect "$WORK/out/rec_youtube.mp4" a:0 codec_name,sample_rate "aac,48000"

conv --preset resolve
expect "$WORK/out/rec_resolve.mov" v:0 codec_name,width,height,pix_fmt "dnxhd,800,960,yuv422p"
expect "$WORK/out/rec_resolve.mov" a:0 codec_name,sample_rate "pcm_s16le,48000"

conv --preset lossless
expect "$WORK/out/rec_lossless.mkv" v:0 codec_name,width,height "ffv1,400,480"
expect "$WORK/out/rec_lossless.mkv" a:0 codec_name "flac"

conv --preset youtube --layout top
expect "$WORK/out/rec_youtube.mp4" v:0 width,height "2400,1440"
conv --preset youtube --layout side
expect "$WORK/out/rec_youtube.mp4" v:0 width,height "3600,1200"
conv --preset resolve --layout split
expect "$WORK/out/rec_resolve_top.mov" v:0 width,height "1600,960"
expect "$WORK/out/rec_resolve_bottom.mov" v:0 width,height "1280,960"

# Screen positions, exact pixels: the last frame of a lossless file at 1x
pix() {   # file x y -> "r g b"
    ffmpeg -v error -sseof -0.2 -i "$1" -frames:v 1 -f rawvideo -pix_fmt rgb24 - | python3 -c "
import sys
d = sys.stdin.buffer.read(); w = $2
x, y = $3, $4
i = (y * w + x) * 3
print(d[i], d[i + 1], d[i + 2])"
}
check_px() {   # file width x y want
    local got; got="$(pix "$1" "$2" "$3" "$4")"
    [ "$got" = "$5" ] || fail "$(basename "$1") pixel ($3,$4): got '$got', want '$5'"
    echo "ok  $(basename "$1")  ($3,$4) = $got"
}
conv --preset lossless
check_px "$WORK/out/rec_lossless.mkv" 400 0 0 "255 0 0"        # top: red at top-left
check_px "$WORK/out/rec_lossless.mkv" 400 399 239 "0 0 255"    # top: blue at bottom-right
check_px "$WORK/out/rec_lossless.mkv" 400 40 240 "0 255 0"     # bottom: green, centred under it
conv --preset lossless --layout side
check_px "$WORK/out/rec_lossless.mkv" 720 400 0 "0 255 0"      # bottom on the right
conv --preset lossless --layout bottom
check_px "$WORK/out/rec_lossless.mkv" 320 0 0 "0 255 0"
conv --preset lossless --scale 3 --layout top
check_px "$WORK/out/rec_lossless.mkv" 1200 23 23 "255 0 0"     # 8 px -> 24 px, nearest neighbour
[ "$(pix "$WORK/out/rec_lossless.mkv" 1200 24 24)" != "255 0 0" ] || fail "upscale: red marker larger than 24 px"
echo "ok  rec_lossless.mkv  (24,24) is past the marker"
echo "ALL PCCONV TESTS PASSED"
