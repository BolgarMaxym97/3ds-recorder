#!/usr/bin/env python3
"""Analyses an audio_debug recording (config.ini: audio_debug=on).

Usage: tools/dspraw.py recording.r3d

In that mode the recorder stores, every 250 us poll where either DSP region's
final-mix block changed, both blocks as read (R3DA records flagged
R3D_AUDIO_RAW). This prints, per region, how the block changes between polls:
time between changes, samples changed per change, and whether the changed
runs are aligned to 8 stereo samples (32 bytes = one ARM11 cache line).

Finding on O3DS (2026-11-03): changes came as 32-byte aligned lines in random
order, i.e. the recorder read the cached mapping without invalidating it. With
the invalidation in place a frame should appear as one whole-block change.
"""
import collections
import struct
import sys

MAGIC_AUDIO = 0x41443352
KNOWN = (0x50443352, 0x4D443352, MAGIC_AUDIO, 0x59443352, 0x46443352, 0x58443352)
RAW = 2


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    d = open(sys.argv[1], "rb").read()
    pos = 32
    reads = {0: [], 1: []}
    while pos + 16 <= len(d):
        m, ln, t, _, region, _ = struct.unpack_from("<IIIBBH", d, pos)
        if m not in KNOWN:
            break
        stored = (ln + 15) & ~15 if m in (0x58443352, 0x59443352) else ln
        if m == MAGIC_AUDIO and ln >= 656 and struct.unpack_from("<H", d, pos + 16 + 646)[0] & RAW:
            reads[region & 1].append((t, struct.unpack_from("<160I", d, pos + 16)))
        pos += 16 + stored
    if not reads[0] and not reads[1]:
        raise SystemExit("no raw reads: not an audio_debug recording")
    for r in (0, 1):
        dts, sizes, aligned, runs_total = collections.Counter(), collections.Counter(), 0, 0
        prev = last_t = None
        for t, x in reads[r]:
            if prev is not None and x != prev:
                ch = [i for i in range(160) if x[i] != prev[i]]
                dts[min((t - last_t) // 500, 20)] += 1
                sizes[len(ch) // 32 * 32] += 1
                runs, start, p = [], ch[0], ch[0]
                for i in ch[1:]:
                    if i != p + 1:
                        runs.append((start, p))
                        start = i
                    p = i
                runs.append((start, p))
                for a, b in runs:
                    if b - a + 1 >= 8:
                        runs_total += 1
                        aligned += a % 8 == 0 and (b + 1) % 8 == 0
                last_t = t
            elif prev is None:
                last_t = t
            prev = x
        n = sum(dts.values())
        print(f"region {r}: {n} changes")
        print("  ms between changes (x0.5):", sorted(dts.items()))
        print("  samples changed per change (bins of 32):", sorted(sizes.items()))
        if runs_total:
            print(f"  runs of >= 8 samples aligned to 8 (cache lines): {100 * aligned / runs_total:.0f}%")


if __name__ == "__main__":
    main()
