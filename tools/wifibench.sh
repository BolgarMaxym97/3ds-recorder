#!/usr/bin/env bash
# Measures download speed from the 3DS (tests/wifibench running on it) over Wi-Fi,
# with 1-4 connections at once: total speed = all bytes / wall time.
# Usage: tools/wifibench.sh <3ds-ip>
set -euo pipefail
IP="${1:?usage: tools/wifibench.sh <3ds-ip>}"
URL="http://$IP:8080"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# label, then one URL path per parallel connection
par() {
    local label="$1"; shift
    local i=0 t0 t1
    t0=$(python3 -c 'import time; print(time.time())')
    for path in "$@"; do
        curl -s -o /dev/null --max-time 600 -w '%{size_download}\n' "$URL$path" >"$TMP/$i" &
        i=$((i + 1))
    done
    wait
    t1=$(python3 -c 'import time; print(time.time())')
    cat "$TMP"/[0-9]* | awk -v l="$label" -v n="$#" -v t="$(echo "$t1 - $t0" | bc)" \
        '{ s += $1 } END { printf "%-34s %d conn %7.1f MB %6.1f s %7.0f KB/s\n", l, n, s / 1048576, t, s / 1024 / t }'
    rm -f "$TMP"/[0-9]*
}

echo "3DS at $IP"
par "memory 16K"             "/zero?mb=8&kb=16"
par "memory 16K"             "/zero?mb=4&kb=16" "/zero?mb=4&kb=16"
par "memory 16K"             "/zero?mb=3&kb=16" "/zero?mb=3&kb=16" "/zero?mb=3&kb=16"
par "memory 16K"             "/zero?mb=2&kb=16" "/zero?mb=2&kb=16" "/zero?mb=2&kb=16" "/zero?mb=2&kb=16"
par "memory 16K sndbuf 64K"  "/zero?mb=8&kb=16&sndbuf=64"
par "memory 16K sndbuf 64K"  "/zero?mb=4&kb=16&sndbuf=64" "/zero?mb=4&kb=16&sndbuf=64"
par "memory 16K (again)"     "/zero?mb=8&kb=16"
par "SD file, 16 MB"         "/file?off=0&mb=16"
par "SD file, 2 x 8 MB"      "/file?off=16&mb=8" "/file?off=24&mb=8"
par "SD file, 4 x 4 MB"      "/file?off=32&mb=4" "/file?off=36&mb=4" "/file?off=40&mb=4" "/file?off=44&mb=4"
