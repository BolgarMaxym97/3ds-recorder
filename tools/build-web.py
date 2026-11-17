#!/usr/bin/env python3
"""Builds the phone page the console serves: web/index.html with every local
<script src="..."></script> inlined (one request, works offline), gzip-compressed
(the console sends it with Content-Encoding: gzip, the 3DS Wi-Fi is slow).
With --local: the same page for GitHub Pages, plain .html, in local mode (a .r3d from the computer,
web/app.js localMain).

Usage: build-web.py [--local] <web dir> <out .html.gz | out .html>   (app/Makefile runs it; .github/workflows/pages.yml with --local)
"""
import gzip
import pathlib
import re
import sys

args = sys.argv[1:]
local = '--local' in args
if local:
    args.remove('--local')
web, out = pathlib.Path(args[0]), pathlib.Path(args[1])
html = (web / 'index.html').read_text(encoding='utf-8')
if local:
    if html.count('<html lang="en">') != 1:
        sys.exit('index.html: no <html lang="en"> to mark as local')
    html = html.replace('<html lang="en">', '<html lang="en" data-mode="local">')


def inline(m):
    src = m.group(1)
    if '://' in src:
        sys.exit(f'{src}: the page must not load anything from the internet')
    js = (web / src).read_text(encoding='utf-8').replace('</script', '<\\/script')
    return f'<script>/* {src} */\n{js}\n</script>'


html = re.sub(r'<script src="([^"]+)"></script>', inline, html)
out.parent.mkdir(parents=True, exist_ok=True)
if local:
    out.write_text(html, encoding='utf-8')
    print(f'{out}: {len(html)} bytes')
    sys.exit(0)
packed = gzip.compress(html.encode('utf-8'), compresslevel=9, mtime=0)
if not out.exists() or out.read_bytes() != packed:  # unchanged: keep the date (the 3dsx is rebuilt on it)
    out.write_bytes(packed)
print(f'{out}: {len(html)} bytes, {out.stat().st_size} gzipped')
