#!/usr/bin/env python3
"""Serves this folder over plain HTTP on the LAN and accepts POST /upload?name=x.mp4
(the encode test sends its MP4 here so it can be inspected with ffprobe).
Usage: python3 tests/webcodecs/serve.py <lan-ip> [port]"""
import http.server, os, sys, urllib.parse

ROOT = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(ROOT, "uploads")


class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=ROOT, **k)

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        if u.path != "/upload":
            self.send_error(404)
            return
        name = os.path.basename(urllib.parse.parse_qs(u.query).get("name", ["upload.bin"])[0])
        n = int(self.headers.get("Content-Length", "0"))
        os.makedirs(OUT, exist_ok=True)
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(self.rfile.read(n))
        self.send_response(200)
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"ok")
        print("saved", name, n, "bytes", flush=True)


http.server.ThreadingHTTPServer((sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 8000), H).serve_forever()
