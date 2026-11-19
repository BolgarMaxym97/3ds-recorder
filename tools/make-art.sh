#!/usr/bin/env bash
# Builds the control app's fonts and HOME Menu art (run on the Mac; output is committed):
#   app/romfs/font/*.bcfnt   tools/make-fonts.sh (Inter, Noto Sans JP/SC/TC: the texts' characters)
#   app/assets/icon.png (48x48), app/assets/banner.png (256x128), banner.wav, banner.bnr
#   app/assets/logo.bcma.lz   the CIA's launch splash (tools/logo.py), signed with the HOME Menu's
#                             key from HOME_MENU_CODE (3DS UA's dump); skipped without the dump
# Needs: Google Chrome (renders the SVGs in app/assets/art), Python with fontTools
# (FONT_PY, default: 3ds-ua-hub's venv), docker image 3dsrec-dev (mkbcfnt), and bannertool
# (BANNERTOOL, default: the build in ../3ds-ua/tmp/tools).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
APP="$REPO/app"
FONT_PY="${FONT_PY:-$REPO/../3ds-ua-hub/.venv/bin/python}"
BANNERTOOL="${BANNERTOOL:-$REPO/../3ds-ua/tmp/tools/bannertool.bin}"
HOME_MENU_CODE="${HOME_MENU_CODE:-$REPO/../3ds-ua/work/0004003000009802/code.bin}"
CHROME="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
TMP="$REPO/build/art"
mkdir -p "$TMP" "$REPO/build/fonts" "$APP/romfs/font"

# fonts: only the characters the texts use (tools/make-fonts.sh)
"$REPO/tools/make-fonts.sh"

# icon and banner: SVG -> PNG with a transparent background
render() { # html, width, height, out
    "$CHROME" --headless=new --disable-gpu --hide-scrollbars --default-background-color=00000000 \
        --window-size="$2,$3" --virtual-time-budget=3000 --screenshot="$4" "file://$1" 2>/dev/null
}
render "$APP/assets/art/icon.html" 48 48 "$APP/assets/icon.png"
render "$APP/assets/art/banner.html" 256 128 "$APP/assets/banner.png"

# banner sound: a short soft two-note chime, 16-bit mono 22050 Hz (HOME plays it on selection)
python3 - "$APP/assets/banner.wav" <<'EOF'
import math, struct, sys, wave
rate, out = 22050, []
for f, start, dur in ((880.0, 0.0, 0.35), (1318.5, 0.12, 0.5)):
    for i in range(int(dur * rate)):
        t = i / rate
        env = min(1.0, t / 0.01) * math.exp(-t * 7)
        idx = int(start * rate) + i
        while len(out) <= idx:
            out.append(0.0)
        out[idx] += 0.28 * env * math.sin(2 * math.pi * f * t)
w = wave.open(sys.argv[1], "wb")
w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, s)) * 32767)) for s in out))
w.close()
EOF

"$BANNERTOOL" makebanner -i "$APP/assets/banner.png" -a "$APP/assets/banner.wav" -o "$APP/assets/banner.bnr"
# launch splash: makerom's stock logo from a throwaway CIA, reworked and signed (as 3ds-ua-hub)
if [ -f "$HOME_MENU_CODE" ]; then
    mkdir -p "$REPO/build/logo"
    "$REPO/tools/dkp.sh" app make cia-stock
    "$FONT_PY" "$REPO/tools/logo.py" render "$REPO/build/logo/stock.cia" "$REPO/build/fonts/Inter-800.ttf" "$REPO/build/logo"
    "$REPO/tools/dkp.sh" . sh -c 'for n in waves logo; do tex3ds -f etc1 -q high -z none -r -o build/logo/$n.etc1 build/logo/$n.png || exit 1; done'
    "$FONT_PY" "$REPO/tools/logo.py" pack "$REPO/build/logo" "$HOME_MENU_CODE"
    cp "$REPO/build/logo/logo.bcma.lz" "$APP/assets/logo.bcma.lz"
else
    echo "no $HOME_MENU_CODE: the CIA keeps makerom's homebrew splash"
fi

ls -la "$APP/assets/icon.png" "$APP/assets/banner.png" "$APP/assets/banner.wav" "$APP/assets/banner.bnr" "$APP/assets/logo.bcma.lz" "$APP/romfs/font"
