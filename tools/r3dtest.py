#!/usr/bin/env python3
"""Inspect a bench recording (rec_last.r3dtest) from bench.3gx.

Checks the stream structure, reports per-screen frame counts and pacing,
and optionally exports frames as PNG images (rotated back to screen layout).

Usage:
  tools/r3dtest.py rec_last.r3dtest                 # summary
  tools/r3dtest.py rec_last.r3dtest --dump out/ --every 30
"""
import argparse
import os
import statistics
import struct
import sys
import zlib

MAGIC_FILE = 0x42443352   # "R3DB"
MAGIC_FRAME = 0x46443352  # "R3DF"
MAGIC_PAD = 0x50443352    # "R3DP"
MAGIC_XFRAME = 0x58443352  # "R3DX": XOR-RLE vs previous frame, screen bit 6 = keyframe
MAGIC_YFRAME = 0x59443352  # "R3DY": XR32 (runs of pixel pairs), screen bit 6 = keyframe
DELTA = (MAGIC_XFRAME, MAGIC_YFRAME)
MODES = ["count_only", "half_top", "half_both", "full_top", "full_both"]
SCREENS = {0: "top", 1: "bottom"}


def xrle_decode(payload, prev, n, key, unit=1):
    """Decodes one XOR-RLE (unit 1) or XR32 (unit 2: counts are pixel pairs)
    frame (codec/source/codec.c) into a list of n words."""
    out = [0] * n if key else list(prev)
    pos = i = 0
    while pos + 2 <= len(payload):
        t = payload[pos] | payload[pos + 1] << 8
        pos += 2
        count = ((t & 0x7FFF) + 1) * unit
        if i + count > n:
            raise ValueError("xrle: run past end of frame")
        if t & 0x8000:
            words = struct.unpack_from(f"<{count}H", payload, pos)
            pos += count * 2
            if key:
                out[i:i + count] = words
            else:
                out[i:i + count] = [a ^ b for a, b in zip(out[i:i + count], words)]
        elif key:
            out[i:i + count] = [0] * count
        i += count
    if i != n or pos != len(payload):
        raise ValueError("xrle: malformed frame")
    return out


def parse(data):
    """Yields (offset, screen, time_us, cols, rows, src_format, payload_view).
    XOR-RLE frames are decoded, so payload is always raw RGB565."""
    if len(data) < 32:
        raise ValueError("file too short")
    magic, version, tid, is_new, mode = struct.unpack_from("<IIQBB", data, 0)
    if magic != MAGIC_FILE:
        raise ValueError(f"bad file magic 0x{magic:08X}")
    header = {"version": version, "tid": f"{tid:016X}", "new3ds": bool(is_new),
              "mode": MODES[mode] if mode < len(MODES) else mode}

    frames = []
    pos = 32
    pads = 0
    prev = {}
    enc_bytes = raw_bytes = 0
    while pos + 16 <= len(data):
        magic, length, time_us, screen, src_fmt, cols = struct.unpack_from("<IIIBBH", data, pos)
        if magic in (0x41443352, 0x4D443352):  # R3DA audio, R3DM DSP snapshot (v4)
            pos += 16 + length
            continue
        if magic == MAGIC_PAD:
            pads += 1
            pos += 16 + length
            continue
        if magic != MAGIC_FRAME and magic not in DELTA:
            raise ValueError(f"bad record magic 0x{magic:08X} at offset {pos}")
        stored = (length + 15) & ~15 if magic in DELTA else length
        if pos + 16 + stored > len(data):
            print(f"warning: truncated last frame at offset {pos}", file=sys.stderr)
            break
        body = memoryview(data)[pos + 16:pos + 16 + length]
        if magic in DELTA:
            key = bool(screen & 0x40)
            screen &= 0x3F
            rows = 120 if cols in (200, 160) else 240
            n = cols * rows
            if not key and screen not in prev:
                raise ValueError(f"delta frame before keyframe at offset {pos}")
            words = xrle_decode(body, prev.get(screen), n, key, 2 if magic == MAGIC_YFRAME else 1)
            prev[screen] = words
            body = struct.pack(f"<{n}H", *words)
            enc_bytes += length
            raw_bytes += n * 2
        else:
            rows = length // (cols * 2) if cols else 0
        frames.append((pos, screen, time_us, cols, rows, src_fmt, body))
        pos += 16 + stored
    if raw_bytes:
        header["xrle_ratio"] = round(raw_bytes / enc_bytes, 2)
    return header, frames, pads, len(data) - pos


def rgb565_to_png(payload, cols, rows, path):
    """Stored rotated: column c holds `rows` pixels bottom-to-top.
    Screen image: width = cols, height = rows, pixel (x=c, y=rows-1-r)."""
    px = struct.unpack(f"<{cols * rows}H", payload)
    raw = bytearray()
    for y in range(rows):
        r = rows - 1 - y
        raw.append(0)  # PNG filter: none
        for c in range(cols):
            v = px[c * rows + r]
            raw += bytes((((v >> 11) & 0x1F) * 255 // 31, ((v >> 5) & 0x3F) * 255 // 63, (v & 0x1F) * 255 // 31))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", cols, rows, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--dump", help="directory for PNG frames")
    ap.add_argument("--every", type=int, default=30, help="export every N-th frame per screen (default 30)")
    args = ap.parse_args()

    with open(args.file, "rb") as f:
        data = f.read()
    header, frames, pads, tail = parse(data)

    print(f"file: {args.file} ({len(data) / 2**20:.1f} MB)")
    print(f"header: {header}")
    print(f"records: {len(frames)} frames, {pads} pads, {tail} trailing bytes")
    for screen in sorted({f[1] for f in frames}):
        fr = [f for f in frames if f[1] == screen]
        times = [f[2] for f in fr]
        deltas = [b - a for a, b in zip(times, times[1:])]
        dur = (times[-1] - times[0]) / 1e6 if len(times) > 1 else 0
        print(f"  {SCREENS.get(screen, screen)}: {len(fr)} frames, {fr[0][3]}x{fr[0][4]} (stored cols x rows), "
              f"src format {fr[0][5]}, {dur:.1f} s, {(len(fr) - 1) / dur if dur else 0:.2f} fps")
        if deltas:
            deltas.sort()
            p = lambda q: deltas[min(len(deltas) - 1, int(q * len(deltas)))] / 1000
            print(f"    interval ms: median {statistics.median(deltas) / 1000:.1f}, p95 {p(0.95):.1f}, "
                  f"p99 {p(0.99):.1f}, max {deltas[-1] / 1000:.1f}; gaps > 50 ms: {sum(d > 50000 for d in deltas)}")

    if args.dump:
        os.makedirs(args.dump, exist_ok=True)
        counters = {}
        written = 0
        for _, screen, time_us, cols, rows, _, payload in frames:
            n = counters.get(screen, 0)
            counters[screen] = n + 1
            if n % args.every:
                continue
            name = f"{SCREENS.get(screen, screen)}_{n:05d}_{time_us // 1000:07d}ms.png"
            rgb565_to_png(payload, cols, rows, os.path.join(args.dump, name))
            written += 1
        print(f"exported {written} frames to {args.dump}")


if __name__ == "__main__":
    main()
