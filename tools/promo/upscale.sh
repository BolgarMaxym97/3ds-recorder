#!/usr/bin/env bash
# Upscales the promo clips 4x with Real-ESRGAN (realesrgan-x4plus), better than prep.sh's ffmpeg upscale.
# Needs realesrgan-ncnn-vulkan (https://github.com/xinntao/Real-ESRGAN/releases, v0.2.5.0 macOS build).
# Usage: tools/promo/upscale.sh [path to realesrgan dir]   — run after prep.sh.
#
# Each clip is upscaled once and kept in build/promo/upscale/<name>.mp4 with its cut in <name>.cut;
# later runs only copy it into public/clips. A clip is upscaled again only when its line in prep.sh
# (source file, start, length) changes.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
RESR="${1:-$REPO/build/promo/tools/resr}"
SRC="$REPO/build/promo/mp4"
WORK="$REPO/build/promo/upscale"
mkdir -p "$WORK"

# Same cut list as prep.sh.
grep -E '^[a-z0-9]+ +2026' "$HERE/prep.sh" | while read -r name file start len; do
    cut="$file $start $len"
    if [ ! -f "$WORK/$name.mp4" ] || [ "$(cat "$WORK/$name.cut" 2>/dev/null)" != "$cut" ]; then
        rm -rf "$WORK/$name" && mkdir -p "$WORK/$name/in" "$WORK/$name/out"
        ffmpeg -nostdin -hide_banner -loglevel error -ss "$start" -i "$SRC/$file" -t "$len" -r 30 "$WORK/$name/in/%05d.png"
        "$RESR/realesrgan-ncnn-vulkan" -i "$WORK/$name/in" -o "$WORK/$name/out" -n realesrgan-x4plus -s 4 -m "$RESR/models" -f png >/dev/null 2>&1
        ffmpeg -nostdin -hide_banner -loglevel error -y -framerate 30 -i "$WORK/$name/out/%05d.png" \
            -c:v libx264 -preset slow -crf 16 -g 15 -pix_fmt yuv420p "$WORK/$name.mp4"
        rm -rf "$WORK/$name"
        echo "$cut" > "$WORK/$name.cut"
        echo "upscaled $name"
    fi
    cp "$WORK/$name.mp4" "$HERE/public/clips/$name.mp4"
done
