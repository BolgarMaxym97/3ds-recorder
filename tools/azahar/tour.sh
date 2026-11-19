#!/bin/bash
# tour.sh [out dir] [language codes...]: every screen of the app in every language (or the given
# ones), as PNGs: builds the TOUR=1 app (app_tour in app.c), runs it in Azahar, which draws each
# screen and saves it to the emulated SD card, and copies the pictures out (default build/tour).
# Azahar's own config.ini on the emulated SD card is kept as it was.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$REPO/build/tour}"
shift || true
AZAHAR="${AZAHAR:-$HOME/Work/Pet/azahar-macos-universal-2126.2/Azahar.app}"
SD="$HOME/Library/Application Support/Azahar/sdmc/3ds/recorder"

"$REPO/tools/dkp.sh" app make TOUR=1 >/dev/null
mkdir -p "$SD" "$OUT"
rm -rf "$SD/tour"
if [ $# -gt 0 ]; then echo "$*" >"$SD/tour.txt"; else rm -f "$SD/tour.txt"; fi
[ -f "$SD/config.ini" ] && cp "$SD/config.ini" "$OUT/.config.ini.bak"
restore() {
    pgrep -f "MacOS/azahar" | xargs kill -9 2>/dev/null || true
    [ -f "$OUT/.config.ini.bak" ] && mv "$OUT/.config.ini.bak" "$SD/config.ini"
    rm -f "$SD/tour.txt"
}
trap restore EXIT

pgrep -f "MacOS/azahar" | xargs kill -9 2>/dev/null || true
while pgrep -f "MacOS/azahar" >/dev/null; do sleep 0.5; done # an exiting one swallows the launch
open -g -a "$AZAHAR" --args "$REPO/app/3ds-recorder-tour.3dsx"
# done when no new picture came for 20 s (after the first one)
last=-1 same=0
for _ in $(seq 1 600); do
    sleep 2
    n=$(ls "$SD/tour" 2>/dev/null | wc -l | tr -d ' ')
    if [ "$n" -gt 0 ] && [ "$n" = "$last" ]; then same=$((same + 1)); else same=0; fi
    last=$n
    [ "$same" -ge 10 ] && break
done
rm -f "$OUT"/*.png
for f in "$SD/tour"/*.bmp; do
    sips -s format png "$f" --out "$OUT/$(basename "${f%.bmp}").png" >/dev/null
done
echo "$(ls "$OUT"/*.png | wc -l | tr -d ' ') screens in $OUT"
