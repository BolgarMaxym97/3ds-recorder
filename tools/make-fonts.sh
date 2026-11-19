#!/usr/bin/env bash
# Builds the control app's fonts (run on the Mac; the output is committed), with only the characters
# the texts use (tools/i18n.py charset), at the em size ui.c draws from (24):
#   app/romfs/font/inter_500.bcfnt, inter_700.bcfnt   Inter: Latin and Cyrillic, and the language
#       list in every language (its Japanese and Chinese characters from Noto Sans JP / SC / TC)
#   app/romfs/font/<ja|zhs|zht>_500.bcfnt, _700        Inter + Noto Sans JP / SC / TC, for a language
#       written with them (loaded while it is on: ui_set_script); only when LANGS has it
# Inter (OFL) is app/assets/fonts/Inter-var.ttf; Noto Sans CJK (OFL) is downloaded, pinned below.
# Needs: Python with fontTools (FONT_PY, default: 3ds-ua-hub's venv), docker image 3dsrec-dev (mkbcfnt).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
FONT_PY="${FONT_PY:-$REPO/../3ds-ua-hub/.venv/bin/python}"
B="$REPO/build/fonts"
OUT="$REPO/app/romfs/font"
mkdir -p "$B/noto" "$OUT"

# google/fonts at this commit: ofl/notosans{jp,sc,tc}/NotoSans{JP,SC,TC}[wght].ttf
NOTO_COMMIT=5bd87dfc525c8a0d1d8529d3684716e00c3fc59a
noto() { # JP|SC|TC sha256
    local f="$B/noto/NotoSans$1.ttf" lower
    lower=$(echo "$1" | tr 'A-Z' 'a-z')
    [ -f "$f" ] || curl -sfL -o "$f" \
        "https://raw.githubusercontent.com/google/fonts/$NOTO_COMMIT/ofl/notosans$lower/NotoSans$1%5Bwght%5D.ttf"
    echo "$2  $f" | shasum -a 256 -c --quiet - || { rm -f "$f"; echo "NotoSans$1: checksum differs" >&2; exit 1; }
}

for w in 500 700; do
    "$FONT_PY" -m fontTools.varLib.instancer "$REPO/app/assets/fonts/Inter-var.ttf" wght=$w opsz=14 -q -o "$B/Inter-$w.ttf"
done
for f in base ja zhs zht; do
    python3 "$REPO/tools/i18n.py" charset $f >"$B/cs-$f.txt"
done
# the Japanese and Chinese characters each font needs (none: no such language yet)
cjk() { awk '$1 >= 11904' "$B/cs-$1.txt"; }

# Japanese and Chinese characters drawn this much larger than Inter's em: at the app's small sizes
# (9-11 px) kanji at 1 em are 8 px high, too few pixels for their strokes. The em is shrunk, so the
# glyphs and their advances grow together; ui.c keeps the line height and the Latin letters as they
# are (font_adj).
CJK_SCALE="${CJK_SCALE:-1.2}"
need_noto() { # JP|SC|TC sha
    noto "$1" "$2"
    for w in 500 700; do
        [ -f "$B/noto/$1-$w-$CJK_SCALE.ttf" ] || {
            "$FONT_PY" -m fontTools.varLib.instancer "$B/noto/NotoSans$1.ttf" wght=$w -q -o "$B/noto/$1-$w.ttf"
            "$FONT_PY" - "$B/noto/$1-$w.ttf" "$CJK_SCALE" "$B/noto/$1-$w-$CJK_SCALE.ttf" <<'PY'
import sys
from fontTools.ttLib import TTFont
f = TTFont(sys.argv[1])
f["head"].unitsPerEm = round(f["head"].unitsPerEm / float(sys.argv[2]))
f.save(sys.argv[3])
PY
        }
    done
}
NOTO_SHA_JP=c2f3b4d463500a2ddcd3849cded1fceeb9fd6d1c32e6cbecd568453ba50fc68f
NOTO_SHA_SC=a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da
NOTO_SHA_TC=864727d210d54f2537bbe23b3a839436c3992af72de9322af5270897246bd44f

# base: Inter, then each Chinese and Japanese language's own characters in its own Noto (a character
# the language list shows in Japanese and in Chinese takes the Japanese form)
python3 - "$B" <<'EOF'
import subprocess, sys
B = sys.argv[1]
names = {"ja": "JP", "zhs": "SC", "zht": "TC"}
taken = set()
for f, n in names.items():
    mine = {int(x) for x in open(f"{B}/cs-base.txt").read().split()} & {int(x) for x in open(f"{B}/cs-{f}.txt").read().split()}
    mine = {c for c in mine if c >= 0x2E80} - taken
    taken |= mine
    open(f"{B}/base-{n}.txt", "w").write("\n".join(str(c) for c in sorted(mine)))
EOF
# A font cut to `charset` before mkbcfnt: mkbcfnt makes every cell as wide as the widest glyph the
# font has (Inter's ligatures are 2.7 em: 11 cells a row instead of 31), so only the glyphs drawn go
# in, without the layout features that would bring others along.
sub() { # src charset-file out
    "$FONT_PY" -m fontTools.subset "$1" --unicodes-file=<(awk '{printf "U+%X\n", $1}' "$2") --output-file="$3" \
        --notdef-outline --recalc-bounds --layout-features=''
}
mk() { # out charset fonts... (paths under build/fonts)
    local out="$1" cs="$2"
    shift 2
    "$REPO/tools/dkp.sh" . sh -c "mkbcfnt -s 24 -w ${cs#"$REPO"/} -o ${out#"$REPO"/} $*" >/dev/null
}
latin() { awk '$1 < 11904' "$1"; }

base_fonts=""
for n in JP SC TC; do
    [ -s "$B/base-$n.txt" ] || continue
    eval "need_noto $n \$NOTO_SHA_$n"
    for w in 500 700; do sub "$B/noto/$n-$w-$CJK_SCALE.ttf" "$B/base-$n.txt" "$B/noto/base-$n-$w.ttf"; done
    base_fonts="$base_fonts $n"
done
for w in 500 700; do
    sub "$B/Inter-$w.ttf" "$B/cs-base.txt" "$B/sub-Inter-$w.ttf"
    extra=""
    for n in $base_fonts; do extra="$extra build/fonts/noto/base-$n-$w.ttf"; done
    mk "$OUT/inter_$w.bcfnt" "$B/cs-base.txt" "build/fonts/sub-Inter-$w.ttf$extra"
done

# a font per Japanese / Chinese language: Inter for the Latin letters, Noto for the rest
for pair in ja:JP zhs:SC zht:TC; do
    f=${pair%:*} n=${pair#*:}
    if [ -z "$(cjk $f)" ]; then
        rm -f "$OUT/${f}_500.bcfnt" "$OUT/${f}_700.bcfnt"
        continue
    fi
    eval "need_noto $n \$NOTO_SHA_$n"
    latin "$B/cs-$f.txt" >"$B/cs-$f-latin.txt"
    cjk $f >"$B/cs-$f-cjk.txt"
    for w in 500 700; do
        sub "$B/Inter-$w.ttf" "$B/cs-$f-latin.txt" "$B/sub-$f-Inter-$w.ttf"
        sub "$B/noto/$n-$w-$CJK_SCALE.ttf" "$B/cs-$f-cjk.txt" "$B/noto/sub-$f-$w.ttf"
        mk "$OUT/${f}_$w.bcfnt" "$B/cs-$f.txt" "build/fonts/sub-$f-Inter-$w.ttf build/fonts/noto/sub-$f-$w.ttf"
    done
done
ls -l "$OUT"
