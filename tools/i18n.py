#!/usr/bin/env python3
"""The app's and the phone page's texts in every language, read from the sources:
app/source/i18n.h (the keys, English), app/source/lang/<code>.c, app/source/i18n.c (LANGS) and
web/app.js (TEXT).

  tools/i18n.py check             every language against English (keys, placeholders, line breaks,
                                  button names) and against its built font (each character drawn);
                                  exit 1 on a problem
  tools/i18n.py charset <font>    the code points a font needs, one per line: base (Inter: the
                                  Latin and Cyrillic languages, and the language list in every
                                  language), ja, zhs, zht (that language, with Latin)
  tools/i18n.py widths <code>     each text's width in pixels next to English's, widest change first
  tools/i18n.py table <code>      English | translation, tab separated (for review)
  tools/i18n.py export <code>     the language's texts alone as JSON {key: text} (app keys, then
                                  "web.<key>"): what a blind back-translation gets
  tools/i18n.py review <code> <back.json>
                                  Markdown: key | English | translation | back-translation, for a reviewer
  tools/i18n.py notes <dir>       the in-app update's "What's new" (<dir>/<code>.txt, one point a
                                  line): English there, at most NOTES_MAX lines of at most
                                  NOTES_LINE characters, every character in that language's built
                                  font; exit 1 on a problem (stats/scripts/update-manifest.mjs runs it)
  tools/i18n.py context           every key with its English and Ukrainian text and the code that draws
                                  it (function, size, width), as Markdown: for translators

Needs only Python 3 (no fontTools): the fonts are read from the built .bcfnt files.
"""
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "app", "source")
FONTS = os.path.join(REPO, "app", "romfs", "font")

C_STR = r'"(?:[^"\\]|\\.)*"'
SCRIPT_FONT = {"SCRIPT_LATIN": "base", "SCRIPT_JA": "ja", "SCRIPT_ZH_HANS": "zhs", "SCRIPT_ZH_HANT": "zht"}
# drawn by the app itself in every language (app.c literals, ui.c): every font has them
ALWAYS = "".join(chr(c) for c in range(0x20, 0x7F)) + "…·≈—–×↑↓←→№’“”«»„"
# shown on the language list in each language's own words (the base font, ui_base_font)
LANG_SCREEN = ["S_LANG_CHOOSE", "S_LANG_CONTINUE", "S_LANGUAGE"]


def c_string(literals):
    """ "a" "b" with C escapes -> str """
    out = ""
    for lit in re.findall(C_STR, literals):
        body = lit[1:-1]
        out += re.sub(r'\\(["\\nt])', lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), body)
    return out


def english():
    src = open(os.path.join(SRC, "i18n.h"), encoding="utf-8").read()
    keys = {}
    for m in re.finditer(r"X\((S_\w+), ((?:" + C_STR + r"\s*)+)\)", src):
        keys[m.group(1)] = c_string(m.group(2))
    return keys


def langs():
    """[(enum, code, name, table symbol or None, font)] in LANGS order"""
    src = open(os.path.join(SRC, "i18n.c"), encoding="utf-8").read()
    out = []
    for m in re.finditer(r'\[(LANG_\w+)\] = \{"([^"]+)", "([^"]+)", (\w+), (SCRIPT_\w+)\}', src):
        out.append((m.group(1), m.group(2), m.group(3), None if m.group(4) == "NULL" else m.group(4),
                    SCRIPT_FONT[m.group(5)]))
    return out


def table(code):
    path = os.path.join(SRC, "lang", code + ".c")
    if code == "en":
        return english()
    src = open(path, encoding="utf-8").read()
    return {m.group(1): c_string(m.group(2)) for m in re.finditer(r"\[(S_\w+)\] = ((?:" + C_STR + r"\s*)+),", src)}


def web():
    """{code: {key: text}} from web/app.js's TEXT"""
    src = open(os.path.join(REPO, "web", "app.js"), encoding="utf-8").read()
    start = src.index("const TEXT = {")
    end = src.index("\n  };", start)
    block = src[start:end]
    out, cur = {}, None
    for line in block.split("\n"):
        m = re.match(r"\s{4}'?([\w-]+)'?: \{\s*$", line)
        if m:
            cur = out.setdefault(m.group(1), {})
            continue
        if cur is None:
            continue
        for k, v in re.findall(r"(\w+): '((?:[^'\\]|\\.)*)'", line):
            cur[k] = re.sub(r"\\(.)", r"\1", v)
    return out


C_FMT = re.compile(r"%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?[sdiuxXc%]")
WEB_FMT = re.compile(r"%\d")
BUTTONS = re.compile(r"\b(?:HOME|SELECT|START|Wi-Fi|SD|LED|Luma|Rosalina|MP4|\.r3d|\.cxi)\b|(?<!\w)/(?:3ds|luma)\b[\w/.-]*")


def bcfnt_chars(path):
    """the code points a .bcfnt maps (its CMAP sections)"""
    d = open(path, "rb").read()
    chars = set()
    i = 0
    while True:
        i = d.find(b"CMAP", i)
        if i < 0:
            return chars
        begin, end, method = struct.unpack_from("<HHH", d, i + 8)
        data = i + 20
        if method == 0:  # direct
            chars.update(range(begin, end + 1))
        elif method == 1:  # table: 0xFFFF = none
            for k in range(end - begin + 1):
                if struct.unpack_from("<H", d, data + 2 * k)[0] != 0xFFFF:
                    chars.add(begin + k)
        elif method == 2:  # scan
            (n,) = struct.unpack_from("<H", d, data)
            for k in range(n):
                chars.add(struct.unpack_from("<H", d, data + 2 + 4 * k)[0])
        i += 4


def bcfnt_widths(path):
    """{code point: advance in citro2d units at FONT_EM 24 (charWidth * 30 / cellHeight)}"""
    d = open(path, "rb").read()
    tglp = d.find(b"TGLP")
    cell_h = d[tglp + 9]
    widths = {}
    i = 0
    while True:
        i = d.find(b"CWDH", i)
        if i < 0:
            break
        first, last = struct.unpack_from("<HH", d, i + 8)
        for k in range(last - first + 1):
            widths[first + k] = d[i + 16 + 3 * k + 2] * 30.0 / cell_h
        i += 4
    index = {}
    i = 0
    while True:
        i = d.find(b"CMAP", i)
        if i < 0:
            break
        begin, end, method = struct.unpack_from("<HHH", d, i + 8)
        data = i + 20
        if method == 0:
            (off,) = struct.unpack_from("<H", d, data)
            for k in range(end - begin + 1):
                index[begin + k] = off + k
        elif method == 1:
            for k in range(end - begin + 1):
                (g,) = struct.unpack_from("<H", d, data + 2 * k)
                if g != 0xFFFF:
                    index[begin + k] = g
        elif method == 2:
            (n,) = struct.unpack_from("<H", d, data)
            for k in range(n):
                cp, g = struct.unpack_from("<HH", d, data + 2 + 4 * k)
                index[cp] = g
        i += 4
    return {cp: widths.get(g, 0.0) for cp, g in index.items()}


def charset(font):
    chars = set(ALWAYS)
    all_langs = langs()
    for _, code, name, _, f in all_langs:
        t = table(code)
        if f == font:
            for s in t.values():
                chars.update(s)
            chars.update(name)
        if font == "base":  # the language list: names and its screen in each language
            chars.update(name)
            for k in LANG_SCREEN:
                chars.update(t.get(k, ""))
    chars.discard("\n")
    return sorted(ord(c) for c in chars)


def check():
    en = english()
    problems = []
    web_texts = web()
    for _, code, name, _, font in langs():
        if code == "en":
            continue
        t = table(code)
        for k in en:
            if k not in t:
                problems.append(f"{code} {k}: missing (shows English)")
        for k in t:
            if k not in en:
                problems.append(f"{code} {k}: not a key")
        for k, s in t.items():
            if k not in en:
                continue
            e = en[k]
            if C_FMT.findall(s) != C_FMT.findall(e):
                problems.append(f"{code} {k}: placeholders {C_FMT.findall(s)} != English {C_FMT.findall(e)}")
            if s.count("\n") != e.count("\n"):
                problems.append(f"{code} {k}: {s.count(chr(10))} line breaks, English has {e.count(chr(10))}")
            for b in {x.rstrip(".") for x in BUTTONS.findall(e)}:
                if b not in s and not b.startswith("/"):
                    pass  # names may be translated (HOME Menu wording); paths may not
                if b.startswith("/") and b not in s:
                    problems.append(f"{code} {k}: path {b} is missing")
            if s != s.strip() or "  " in s:
                problems.append(f"{code} {k}: spaces at an end or doubled")
        # every character in the font that draws it
        path = os.path.join(FONTS, ("inter" if font == "base" else font) + "_500.bcfnt")
        if os.path.exists(path):
            have = bcfnt_chars(path)
            need = set()
            for s in t.values():
                need.update(ord(c) for c in s if c != "\n")
            need.update(ord(c) for c in name)
            missing = sorted(need - have)
            if missing:
                problems.append(f"{code}: {len(missing)} characters not in {os.path.basename(path)}: "
                                + "".join(chr(c) for c in missing[:40]))
        else:
            problems.append(f"{code}: font {os.path.basename(path)} not built (tools/make-fonts.sh)")
        # the phone page
        w = web_texts.get(code)
        if w is None:
            problems.append(f"{code}: no TEXT.{code} in web/app.js")
        else:
            for k, e in web_texts["en"].items():
                if k not in w:
                    problems.append(f"{code} web.{k}: missing")
                elif sorted(WEB_FMT.findall(w[k])) != sorted(WEB_FMT.findall(e)):
                    problems.append(f"{code} web.{k}: placeholders differ from English")
    # the language list draws every name and its screen in the base font
    base = os.path.join(FONTS, "inter_500.bcfnt")
    have = bcfnt_chars(base)
    for _, code, name, _, _ in langs():
        t = table(code)
        for s in [name] + [t.get(k, "") for k in LANG_SCREEN]:
            miss = {c for c in s if ord(c) not in have}
            if miss:
                problems.append(f"{code}: language list text {s!r} needs {''.join(sorted(miss))} in the base font")
    for p in problems:
        print(p)
    print(f"i18n: {len(problems)} problem(s), {len(langs())} languages, {len(en)} keys")
    return 1 if problems else 0


def widths(code):
    font = {c: f for _, c, _, _, f in langs()}[code]
    wb = bcfnt_widths(os.path.join(FONTS, "inter_500.bcfnt"))
    wl = wb if font == "base" else bcfnt_widths(os.path.join(FONTS, font + "_500.bcfnt"))
    en, t = english(), table(code)
    rows = []
    for k, e in en.items():
        if k not in t:
            continue
        we = max(sum(wb.get(ord(c), 0) for c in line) for line in e.split("\n"))
        wt = max(sum(wl.get(ord(c), 0) for c in line) for line in t[k].split("\n"))
        rows.append((wt / we if we else 1, k, we, wt))
    for r, k, we, wt in sorted(rows, reverse=True):
        print(f"{r:5.2f}  {we:7.0f} -> {wt:7.0f}  {k}")


def context():
    """[(key, en, uk, [(function, code line)])]"""
    en, uk = english(), table("uk")
    uses = {k: [] for k in en}
    for name in sorted(os.listdir(SRC)):
        if not name.endswith(".c") or name == "i18n.c":
            continue
        lines = open(os.path.join(SRC, name), encoding="utf-8").read().split("\n")
        func = None
        for i, line in enumerate(lines):
            m = re.match(r"^(?:static\s+)?(?:const\s+)?[\w\s*]+?\b(\w+)\(.*\)\s*$", line)
            if m and i + 1 < len(lines) and lines[i + 1].startswith("{"):
                func = m.group(1)
            for k in set(re.findall(r"\bS_[A-Z0-9_]+\b", line)):
                if k in uses and len(uses[k]) < 4:
                    uses[k].append((f"{name}: {func}", line.strip()))
    wb = bcfnt_widths(os.path.join(FONTS, "inter_500.bcfnt"))
    out = ["# The app's texts, with where they are drawn", "",
           "Generated by `tools/i18n.py context`. Sizes in the code are the em in screen pixels (the top screen is",
           "400 x 240, the bottom 320 x 240); `ui_text_wrap(x, y, width, px, ...)` wraps at `width`. `width at 10 px` is",
           "how wide the English text is at a 10 px em: a translation much wider than that needs a look in the tour.",
           ""]
    for k, e in en.items():
        w = max(sum(wb.get(ord(c), 0) for c in line) for line in e.split("\n")) * 10 / 24
        out.append(f"## {k}")
        out.append(f"- en: {json_str(e)}")
        out.append(f"- uk: {json_str(uk.get(k, ''))}")
        ph = C_FMT.findall(e)
        if ph:
            out.append(f"- placeholders, in this order: {', '.join('`' + p + '`' for p in ph)}")
        out.append(f"- width at 10 px: {w:.0f}")
        for f, line in uses[k]:
            out.append(f"- `{f}`: `{line[:160]}`")
        out.append("")
    out += ["# The phone page (web/app.js TEXT)", "",
            "Shown in the phone's or computer's browser (system fonts, no width limit like the console's, but a",
            "phone screen is narrow). `%1` is filled in by the page. Keys are `web.<key>` in export/review.", ""]
    w = web()
    for k, e in w["en"].items():
        out += [f"## web.{k}", f"- en: {json_str(e)}", f"- uk: {json_str(w.get('uk', {}).get(k, ''))}", ""]
    return "\n".join(out)


def texts(code):
    """{key: text} of a language: the app's, then the phone page's as web.<key>"""
    out = dict(table(code)) if code != "en" else dict(english())
    for k, v in web().get(code, {}).items():
        out["web." + k] = v
    return out


def review(code, back_path):
    import json
    en, tr = texts("en"), texts(code)
    back = json.load(open(back_path, encoding="utf-8"))
    out = [f"| key | English | {code} | back-translation |", "|---|---|---|---|"]
    cell = lambda s: (s or "").replace("|", "\\|").replace("\n", "<br>")
    for k, e in en.items():
        out.append(f"| {k} | {cell(e)} | {cell(tr.get(k))} | {cell(back.get(k))} |")
    return "\n".join(out)


def json_str(s):
    import json
    return json.dumps(s, ensure_ascii=False)


NOTES_MAX = 6
NOTES_LINE = 120


def notes(folder):
    """problems of a release's "What's new" texts (see the docstring), printed; the exit code"""
    problems = []
    codes = {code: font for _, code, _, _, font in langs()}
    found = {f[:-4] for f in os.listdir(folder) if f.endswith(".txt")}
    if "en" not in found:
        problems.append("en.txt missing: English is what every other language falls back to")
    for code in sorted(found):
        if code not in codes:
            problems.append(f"{code}.txt: no such language (codes: {', '.join(codes)})")
            continue
        text = open(os.path.join(folder, code + ".txt"), encoding="utf-8").read()
        lines = [line.strip() for line in text.splitlines() if line.strip()]
        if not lines:
            problems.append(f"{code}.txt: empty")
        if len(lines) > NOTES_MAX:
            problems.append(f"{code}.txt: {len(lines)} lines, at most {NOTES_MAX}")
        font = codes[code]
        have = bcfnt_chars(os.path.join(FONTS, ("inter" if font == "base" else font) + "_500.bcfnt"))
        for n, line in enumerate(lines, 1):
            if len(line) > NOTES_LINE:
                problems.append(f"{code}.txt:{n}: {len(line)} characters, at most {NOTES_LINE}")
            if "|" in line or "=" in line[:1]:
                problems.append(f"{code}.txt:{n}: '|' separates the lines in the manifest")
            missing = sorted({c for c in line if ord(c) not in have})
            if missing:
                problems.append(f"{code}.txt:{n}: not in the {font} font: {''.join(missing)}")
    for p in problems:
        print(p)
    print(f"notes: {len(problems)} problem(s), {len(found)} language(s)")
    return 1 if problems else 0


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    if cmd == "check":
        sys.exit(check())
    if cmd == "charset":
        print("\n".join(str(c) for c in charset(sys.argv[2])))
    elif cmd == "widths":
        widths(sys.argv[2])
    elif cmd == "export":
        import json
        print(json.dumps(texts(sys.argv[2]), ensure_ascii=False, indent=1))
    elif cmd == "review":
        print(review(sys.argv[2], sys.argv[3]))
    elif cmd == "notes":
        sys.exit(notes(sys.argv[2]))
    elif cmd == "context":
        print(context())
    elif cmd == "table":
        en, t = english(), table(sys.argv[2])
        for k, e in en.items():
            print(f"{k}\t{e!r}\t{t.get(k, '')!r}")
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main()
