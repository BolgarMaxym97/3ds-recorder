#!/usr/bin/env bash
# Cuts the console recordings into the short clips the promo uses and copies the music.
# Clips are cleaned a little (light denoise) and upscaled 4x so the screens stay sharp in close-ups.
# Input: MP4s made from .r3d files (tools/r3d2mp4.sh), 400x480 (top screen over the bottom one).
# Usage: tools/promo/prep.sh [mp4 dir] [music file]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SRC="${1:-$REPO/build/promo/mp4}"
MUSIC="${2:-$REPO/build/promo/music/fulltheme.ogg}"
OUT="$HERE/public"
mkdir -p "$OUT/clips"

# name  source file             start(s)  length(s)
CLIPS="
home     20261124_204700.mp4     0    6
mk7a     20261124_204937.mp4     54   5
mk7b     20261124_204937.mp4     72   5
mk7c     20261124_204937.mp4     84   3
smash    20261124_205234.mp4     6    5
smash2   20261124_205234.mp4     27   2
kirby    20261124_205841.mp4     3    5
kirby2   20261124_205841.mp4     18   2
persona  20261124_210509.mp4     0    5
homemenu 20261124_205410.mp4     0    5
gallery  20261124_210623.mp4     5    5
qr       20261124_210623.mp4     9    6
phone    20261124_204937.mp4     60   7
hotkey   20261125_093155.mp4     2    8
gallist  20261125_093306.mp4     3.95 2.7
"

echo "$CLIPS" | while read -r name file start len; do
    [ -z "$name" ] && continue
    ffmpeg -nostdin -hide_banner -loglevel error -y -ss "$start" -i "$SRC/$file" -t "$len" -an \
        -vf "hqdn3d=2:1.5:0:0,scale=1600:1920:flags=lanczos,unsharp=5:5:0.8" \
        -c:v libx264 -preset slow -crf 16 -g 15 -pix_fmt yuv420p -r 30 "$OUT/clips/$name.mp4"
    echo "clip $name"
done

ffmpeg -hide_banner -loglevel error -y -i "$MUSIC" -t 50 -c:a aac -b:a 192k "$OUT/music.m4a"
echo "music done"
# A soft whoosh for the fades, synthesized (pink noise through a band-pass, swelling in and out), no third-party sound.
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "anoisesrc=color=pink:duration=0.8:sample_rate=48000:amplitude=0.9" \
    -af "bandpass=f=900:width_type=o:w=2,afade=t=in:d=0.45:curve=exp,afade=t=out:st=0.45:d=0.35,highpass=f=200,volume=1.6" \
    -ac 2 "$OUT/whoosh.wav"
echo "whoosh done"

# The phone's own screen recording (iPhone, HEVC 60 fps): the download finishing, then the race playing
# full screen. Two parts joined with a cut, so the shot stays short.
PHONE_REC="${PHONE_REC:-$REPO/build/promo/phone/phone-raw2.mp4}"
ffmpeg -hide_banner -loglevel error -y -i "$PHONE_REC" -an -filter_complex \
    "[0:v]trim=26.5:31.5,setpts=PTS-STARTPTS[a];[0:v]trim=34.5:37.5,setpts=PTS-STARTPTS[b];[a][b]concat=n=2:v=1,fps=30,scale=720:-2:flags=lanczos" \
    -c:v libx264 -preset slow -crf 16 -g 15 -pix_fmt yuv420p "$OUT/clips/phonerec.mp4"
echo "phone recording done"

# 3D models (glTF from Sketchfab, CC-BY-4.0; credits in README)
MODELS="${MODELS:-$REPO/build/promo/models}"
mkdir -p "$OUT/models"
cp -R "$MODELS/nintendo_3ds" "$MODELS/apple_iphone_13_pro_max" "$OUT/models/"
echo "models done"
