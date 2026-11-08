#!/usr/bin/env bash
# Converts a 3DS Recorder .r3d file to an .mp4 (H.264 + AAC when the recording has audio).
# Usage: tools/r3d2mp4.sh <in.r3d> [out.mp4]
# Needs ffmpeg (macOS: brew install ffmpeg). Builds tools/r3dconv on first use.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
IN="${1:?usage: tools/r3d2mp4.sh <in.r3d> [out.mp4]}"
OUT="${2:-${IN%.*}.mp4}"
CONV="$REPO/build/r3dconv"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "ffmpeg not found. Install it once: brew install ffmpeg" >&2
    exit 1
fi
if [ ! -x "$CONV" ] || [ "$REPO/tools/r3dconv/r3dconv.c" -nt "$CONV" ] || [ "$REPO/tools/r3dconv/audio_place.h" -nt "$CONV" ] \
    || [ "$REPO/tools/r3dconv/audio_order.h" -nt "$CONV" ] \
    || [ "$REPO/tools/r3dconv/audio_eq.h" -nt "$CONV" ] \
    || [ "$REPO/codec/include/r3d_format.h" -nt "$CONV" ]; then
    mkdir -p "$REPO/build"
    cc -std=c11 -O2 -Wall -I"$REPO/codec/include" -o "$CONV" \
        "$REPO/tools/r3dconv/r3dconv.c" "$REPO/codec/source/codec.c" -lm
fi

# Pass 1: summary (first stderr line "WxH fps") and the audio track.
WAV="$(mktemp -t r3daudio).wav"
trap 'rm -f "$WAV"' EXIT
INFO="$("$CONV" --info --wav "$WAV" "$IN" 2>&1)"
echo "$INFO" >&2
read -r SIZE FPS < <(echo "$INFO" | head -1)
AUDIO_IN=()
AUDIO_OUT=(-an)
if echo "$INFO" | grep -q '^audio: [1-9]'; then
    AUDIO_IN=(-i "$WAV")
    AUDIO_OUT=(-c:a aac -b:a 160k -ar 48000)
fi
# Pass 2: video, muxed with the audio.
"$CONV" "$IN" | ffmpeg -hide_banner -loglevel warning -y \
    -f rawvideo -pix_fmt rgb24 -s "$SIZE" -r "$FPS" -i - ${AUDIO_IN[@]+"${AUDIO_IN[@]}"} \
    -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p ${AUDIO_OUT[@]+"${AUDIO_OUT[@]}"} -shortest -movflags +faststart "$OUT"
echo "written: $OUT"
