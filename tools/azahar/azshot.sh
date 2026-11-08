#!/bin/bash
# azshot.sh out.png : captures Azahar's main window (works when covered); retries blank captures
D="$(dirname "$0")"
for i in 1 2 3 4 5 6; do
  id=$("$D/azwin"); [ -n "$id" ] || { sleep 1; continue; }
  screencapture -x -o -l"$id" "$1" 2>/dev/null
  w=$(sips -g pixelWidth "$1" 2>/dev/null | awk '/pixelWidth/{print $2}')
  [ "${w:-0}" -gt 1200 ] && exit 0
  sleep 1
done
exit 1
