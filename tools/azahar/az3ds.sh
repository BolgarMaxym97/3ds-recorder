#!/bin/bash
# az3ds.sh capture.png out.png : the two screens from an azshot.sh capture (3412 x 1504, Azahar's
# default layout) as a 400 x 480 picture like the console's: the top screen, the bottom one under it
# centred on black (the README screenshots, docs/screenshots).
set -euo pipefail
ffmpeg -v error -y -i "$1" -filter_complex \
  "[0]crop=1148:688:1132:65,scale=400:240:flags=area[t];[0]crop=918:689:1247:753,scale=320:240:flags=area,pad=400:240:40:0:black[b];[t][b]vstack" \
  -frames:v 1 "$2"
