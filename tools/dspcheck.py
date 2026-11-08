#!/usr/bin/env python3
"""Checks the audio of a 3DS Recorder .r3d (version 4) against the DSP snapshots.

Usage: tools/dspcheck.py recording.r3d

1. R3DA frames: do they look like audio? For PCM, neighbouring samples are
   close: mean |x[n] - x[n-1]| / mean |x| ("roughness") is well below 1 for
   sound and about 1.4 for random data.
2. R3DM snapshots (both DSP regions, every 10 s): which 640-byte blocks look
   like stereo PCM? If the recorder's offset (0xA80, azahar layout) is wrong
   for a DSP firmware, the real final mix shows up here at another offset.
"""
import struct
import sys

MAGIC_FILE = 0x42443352
MAGIC_PAD = 0x50443352
MAGIC_AUDIO = 0x41443352
MAGIC_DSPMEM = 0x4D443352
DELTA = (0x58443352, 0x59443352)
FINAL_OFF = 0xA80
REGION = 0x8000
BLOCK = 640


def roughness(block):
    s = struct.unpack("<%dh" % (len(block) // 2), block)
    left, right = s[0::2], s[1::2]
    tot = diff = 0
    for ch in (left, right):
        tot += sum(abs(x) for x in ch)
        diff += sum(abs(ch[i] - ch[i - 1]) for i in range(1, len(ch)))
    if tot == 0:
        return None  # silence
    return diff / tot


def records(path):
    data = open(path, "rb").read()
    magic, version = struct.unpack_from("<II", data, 0)
    if magic != MAGIC_FILE:
        raise SystemExit("not a .r3d file")
    rate = struct.unpack_from("<I", data, 20)[0] if version >= 4 else 0
    pos = 32
    out = []
    while pos + 16 <= len(data):
        m, ln, t, sc, fmt, cols = struct.unpack_from("<IIIBBH", data, pos)
        if m in (MAGIC_PAD, MAGIC_AUDIO, MAGIC_DSPMEM, 0x46443352):
            stored = ln
        elif m in DELTA:
            stored = (ln + 15) & ~15
        else:
            break
        out.append((m, t, fmt, data[pos + 16:pos + 16 + ln]))
        pos += 16 + stored
    return version, rate, out


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    version, rate, recs = records(sys.argv[1])
    print(f"version {version}, audio rate {rate / 1000 if rate else 0} Hz")
    audio = [(t, reg, p) for m, t, reg, p in recs if m == MAGIC_AUDIO]
    snaps = [(t, p) for m, t, _, p in recs if m == MAGIC_DSPMEM]
    if audio:
        rough = [roughness(p[:BLOCK]) for _, _, p in audio]
        sound = sorted(r for r in rough if r is not None)
        print(f"R3DA: {len(audio)} frames, {len(audio) - len(sound)} silent, regions "
              f"{sum(1 for _, r, _ in audio if r == 0)}/{sum(1 for _, r, _ in audio if r == 1)}")
        if sound:
            med = sound[len(sound) // 2]
            print(f"  roughness median {med:.2f} (sound < 0.6, random ~1.4) -> "
                  f"{'LOOKS LIKE AUDIO' if med < 0.6 else 'NOT AUDIO: offset probably wrong'}")
        trailers = [struct.unpack_from("<HHHH", p, BLOCK) for _, _, p in audio if len(p) >= BLOCK + 16]
        if trailers:
            # frames between two taken frames by the higher region counter: 1 = none missed
            steps = {}
            for i in range(1, len(trailers)):
                a, b = max(trailers[i - 1][:2]), max(trailers[i][:2])
                d = (b - a) & 0xFFFF
                k = d if d <= 4 else "5+"
                steps[k] = steps.get(k, 0) + 1
            poll = sorted(t[2] for t in trailers)
            print(f"  counter step between frames: {dict(sorted(steps.items(), key=str))}")
            print(f"  audio poll gap us: median {poll[len(poll) // 2]}, p99 {poll[int(len(poll) * 0.99)]}, "
                  f"max {poll[-1]}; both-changed {sum(1 for t in trailers if t[3] & 1)}")
        gaps = [audio[i][0] - audio[i - 1][0] for i in range(1, len(audio))]
        if gaps:
            gaps.sort()
            print(f"  frame interval us: median {gaps[len(gaps) // 2]}, p99 {gaps[int(len(gaps) * 0.99)]}, "
                  f"max {gaps[-1]} (DSP frame = 4889)")
    else:
        print("R3DA: no audio frames")
    print(f"R3DM: {len(snaps)} snapshots")
    for t, p in snaps:
        line = []
        for r in range(2):
            reg = p[r * REGION:(r + 1) * REGION]
            counter = struct.unpack_from("<H", reg, REGION - 2)[0]
            cands = []
            for off in range(0, REGION - BLOCK, 16):
                rg = roughness(reg[off:off + BLOCK])
                if rg is not None and rg < 0.5:
                    cands.append((rg, off))
            cands.sort()
            mark = roughness(reg[FINAL_OFF:FINAL_OFF + BLOCK])
            best = ", ".join(f"0x{o:04X}:{rg:.2f}" for rg, o in cands[:4]) or "none"
            line.append(f"r{r} counter {counter:5d} final@0x{FINAL_OFF:X} "
                        f"{'silent' if mark is None else f'{mark:.2f}'} | smooth blocks: {best}")
        print(f"  t={t / 1e6:6.1f}s  " + "\n             ".join(line))


if __name__ == "__main__":
    main()
