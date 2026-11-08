#!/usr/bin/env python3
"""Builds the phone page the console serves: web/index.html with every local
<script src="..."></script> inlined (one request, works offline), gzip-compressed
(the console sends it with Content-Encoding: gzip, the 3DS Wi-Fi is slow).

Usage: build-web.py <web dir> <out .html.gz>   (app/Makefile runs it)
"""
import gzip
import pathlib
import re
import sys

web, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
html = (web / 'index.html').read_text(encoding='utf-8')


def inline(m):
    src = m.group(1)
    if '://' in src:
        sys.exit(f'{src}: the page must not load anything from the internet')
    js = (web / src).read_text(encoding='utf-8').replace('</script', '<\\/script')
    return f'<script>/* {src} */\n{js}\n</script>'


html = re.sub(r'<script src="([^"]+)"></script>', inline, html)
out.parent.mkdir(parents=True, exist_ok=True)
packed = gzip.compress(html.encode('utf-8'), compresslevel=9, mtime=0)
if not out.exists() or out.read_bytes() != packed:  # unchanged: keep the date (the 3dsx is rebuilt on it)
    out.write_bytes(packed)
print(f'{out}: {len(html)} bytes, {out.stat().st_size} gzipped')
