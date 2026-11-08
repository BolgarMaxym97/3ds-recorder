#!/usr/bin/env python3
"""Summarizes a recprobe (phase S spike) run: sdmc:/3ds/recorder/spike/{info.txt,sec.csv,cap.csv}.

Usage: tools/spikecheck.py <spike dir>

Prints the info file, then per 15 s phase: what was on screen (VRAM = HOME /
applet / VRAM game, FCRAM = app framebuffer + owner pid), detected fps of both
screens, captures vs drops by reason, the longest gap of the poll thread (does
the module keep running in HOME?), and encode/DMA timings from cap.csv.
"""
import csv
import statistics
import sys
from pathlib import Path


def region(pa):
    pa = int(pa, 16)
    if 0x18000000 <= pa < 0x18600000:
        return "VRAM"
    if 0x20000000 <= pa < 0x30000000:
        return "FCRAM"
    return f"?{pa:08X}"


def main():
    d = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    info = d / "info.txt"
    if info.exists():
        print(info.read_text().strip(), "\n")
    rows = list(csv.DictReader(open(d / "sec.csv")))
    caps = list(csv.DictReader(open(d / "cap.csv"))) if (d / "cap.csv").exists() else []

    print(f"{'phase':>9} {'mode':6} {'screen top':18} {'fps t/b':>11} {'cap t/b':>9} "
          f"{'busy':>5} {'dmaF':>5} {'noOwn':>5} {'bskip':>5} {'thr':>4} {'gap ms':>6} {'ring KB':>7}")
    by_phase = {}
    for r in rows:
        key = int(r["sec"]) // 15
        by_phase.setdefault(key, []).append(r)
    for key, rs in sorted(by_phase.items()):
        n = len(rs)
        s = lambda k: sum(int(r[k]) for r in rs)
        regions = sorted({f"{region(r['pa_top'])}{'/pid' + r['pid_top'] if r['pid_top'] != '0' else ''}" for r in rs})
        print(f"{key * 15:>4}-{key * 15 + n:<4} {'REC' if rs[0]['phase'] == '1' else 'detect':6} "
              f"{','.join(regions)[:18]:18} {s('det_top') / n:5.1f}/{s('det_bot') / n:<5.1f} "
              f"{s('cap_top'):4}/{s('cap_bot'):<4} {s('busy_top') + s('busy_bot'):5} "
              f"{s('dmafail_top') + s('dmafail_bot'):5} {s('noowner_top') + s('noowner_bot'):5} "
              f"{s('bot_skip'):5} {s('throttle'):4} {max(int(r['max_poll_gap_ms']) for r in rs):6} "
              f"{max(int(r['ring_max_kb']) for r in rs):7}")

    if caps:
        ok = [c for c in caps if int(c["len"]) > 0]
        for scr in (0, 1):
            cs = [c for c in ok if c["screen"] == str(scr)]
            if not cs:
                continue
            enc = [int(c["enc_us"]) for c in cs]
            dma = [int(c["dma_us"]) for c in cs]
            size = [int(c["len"]) for c in cs]
            owners = {(c["owner"], c["pid"], c["fmt"]) for c in cs}
            print(f"\n{'top' if scr == 0 else 'bottom'}: {len(cs)} frames, enc p50 {statistics.median(enc) / 1000:.1f} ms "
                  f"max {max(enc) / 1000:.1f}, dma+enc p50 {statistics.median(dma) / 1000:.1f} ms, "
                  f"mean {statistics.mean(size) / 1024:.0f} KB/frame; owners (owner,pid,fmt): {sorted(owners)}")
        timeouts = sum(1 for c in caps if int(c["flags"]) & 4)
        print(f"DMA timeouts: {timeouts}, failed encodes: {sum(1 for c in caps if int(c['len']) == 0) - timeouts}")


if __name__ == "__main__":
    main()
