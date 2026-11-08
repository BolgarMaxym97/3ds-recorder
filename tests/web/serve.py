#!/usr/bin/env python3
"""Serves the phone page like the console does (app/source/server.c), for browser tests on the Mac:
  GET /            app/romfs/web/index.html.gz (gzip-encoded)
  GET /api/rec     details of the one recording
  GET /rec/<name>  the recording, with Range; --kbs throttles it like the 3DS Wi-Fi (~420 KB/s)
  GET /rec/<stem>.mp4  with --mp4 FILE: the console's prepared MP4, "ready" after --prep-s seconds
                   of "working" in /api/rec (0 = ready at once); --prep failed: the console could not
Details (length, closed, screens, sound) come from build/r3dconv --info.
Usage: tests/web/serve.py <file.r3d> [--port 8090] [--host 0.0.0.0] [--kbs 420] [--lang uk]
                          [--mp4 FILE [--prep-s 10]] [--prep failed]
"""
import argparse, json, os, re, subprocess, sys, time
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

ap = argparse.ArgumentParser()
ap.add_argument('file'); ap.add_argument('--port', type=int, default=8090); ap.add_argument('--host', default='0.0.0.0')
ap.add_argument('--kbs', type=int, default=0); ap.add_argument('--lang', default='uk')
ap.add_argument('--mp4'); ap.add_argument('--prep-s', type=float, default=10); ap.add_argument('--prep')
ap.add_argument('--stall-every', type=int, default=0)  # weak signal: every Nth file request stops halfway
a = ap.parse_args()
repo = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
page = open(os.path.join(repo, 'app/romfs/web/index.html.gz'), 'rb').read()
name = os.path.basename(a.file)
size = os.path.getsize(a.file)
hdr = open(a.file, 'rb').read(32)
out = subprocess.run([os.path.join(repo, 'build/r3dconv'), '--info', a.file], capture_output=True, text=True).stderr
secs = float(re.search(r'^([\d.]+) s,', out, re.M).group(1))
info = {'name': name, 'size': size, 'ms': int(secs * 1000), 'closed': 'end:' in out, 'both': hdr[17] == 4,
        'audio': int.from_bytes(hdr[20:24], 'little') != 0, 'fps': hdr[18], 'ok': True, 'lang': a.lang,
        'version': 'test', 'console': 'Mac test'}

started = time.time()
nreq = 0
def prep_info():
    if a.prep: return {'prep': a.prep, 'prepPct': 0, 'mp4': 0}
    if not a.mp4: return {}
    frac = 1 if a.prep_s <= 0 else (time.time() - started) / a.prep_s
    if frac >= 1: return {'prep': 'ready', 'prepPct': 1000, 'mp4': os.path.getsize(a.mp4)}
    return {'prep': 'working', 'prepPct': int(frac * 1000), 'mp4': 0}

class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def send(self, code, typ, body, extra=()):
        self.send_response(code); self.send_header('Content-Type', typ); self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store'); self.send_header('Connection', 'close')
        for k, v in extra: self.send_header(k, v)
        self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        path = self.path.split('?')[0]
        if path in ('/', '/index.html'):
            return self.send(200, 'text/html; charset=utf-8', page, [('Content-Encoding', 'gzip')])
        if path == '/api/rec':
            return self.send(200, 'application/json', json.dumps({**info, **prep_info()}).encode())
        if path == '/api/log':  # the page's reports: printed, as app.log would get them
            from urllib.parse import parse_qs, urlparse
            print('page:', parse_qs(urlparse(self.path).query).get('m', [''])[0], flush=True)
            return self.send(200, 'text/plain', b'ok')
        if path == '/log.txt':
            return self.send(200, 'text/plain; charset=utf-8', b'3DS Recorder diagnostics (test server)\n',
                             [('Content-Disposition', 'attachment; filename="3ds-recorder-log.txt"')])
        src, size, typ = a.file, globals()['size'], 'application/octet-stream'
        if a.mp4 and path == '/rec/' + name[:-4] + '.mp4' and prep_info().get('prep') == 'ready':
            src, size, typ = a.mp4, os.path.getsize(a.mp4), 'video/mp4'
        elif path != '/rec/' + name:
            return self.send(404, 'text/plain', b'not found')
        m = re.match(r'bytes=(\d+)-(\d*)', self.headers.get('Range', ''))
        lo, hi = (int(m.group(1)), int(m.group(2)) if m and m.group(2) else size - 1) if m else (0, size - 1)
        hi = min(hi, size - 1)
        self.send_response(206 if m else 200); self.send_header('Content-Type', typ)
        self.send_header('Content-Length', str(hi - lo + 1)); self.send_header('Accept-Ranges', 'bytes')
        if m: self.send_header('Content-Range', f'bytes {lo}-{hi}/{size}')
        self.send_header('Connection', 'close'); self.end_headers()
        global nreq
        nreq += 1
        stall = a.stall_every and nreq % a.stall_every == 0
        with open(src, 'rb') as f:
            f.seek(lo); left = hi - lo + 1; t0 = time.time(); sent = 0
            while left:
                if stall and sent >= (hi - lo + 1) // 2:
                    time.sleep(40); return  # the phone must give up on it and ask again
                d = f.read(min(left, 16384)); left -= len(d); sent += len(d)
                try: self.wfile.write(d)
                except BrokenPipeError: return
                if a.kbs:  # 3 requests share it: each gets a third, like the console's link
                    lag = sent / (a.kbs * 1024 / 3) - (time.time() - t0)
                    if lag > 0: time.sleep(lag)
    def do_POST(self):  # test only: the page's result, for ffprobe (tests/web/run.mjs)
        n = int(self.headers.get('Content-Length', '0'))
        data = self.rfile.read(n)
        out = os.path.join(repo, 'build', 'web-result.' + self.path.split('.')[-1].split('?')[0])
        open(out, 'wb').write(data)
        self.send(200, 'text/plain', out.encode())
    def log_message(self, *x): pass

print(f'http://{a.host}:{a.port}/?f={name}  ({size >> 20} MB, {secs:.0f} s)', flush=True)
ThreadingHTTPServer((a.host, a.port), H).serve_forever()
