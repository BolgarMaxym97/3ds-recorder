#!/usr/bin/env python3
"""
r3d2video — 3DS Recorder recordings (.r3d) to high-quality video on a computer.

The console's own MP4 is made for a phone (small, 400 x 480). This makes files for YouTube,
for editing (DaVinci Resolve and others) or for keeping exactly:

  youtube   H.264 + AAC (.mp4). Sharp integer upscale (nearest neighbour) to about 1440p before the
            colour is subsampled, so thin coloured lines and text stay crisp; YouTube also gives
            1440p uploads a higher bitrate.
  resolve   DNxHR HQ + 48 kHz PCM (.mov): an editing format DaVinci Resolve (free version included)
            reads on Windows, macOS and Linux. Upscaled to fit a 1080p timeline.
  lossless  FFV1 RGB + FLAC (.mkv): every pixel and sample as recorded, at 1x. For archiving.

Layouts: both (top above bottom, as recorded), top, bottom, side (side by side), split (one file
per screen).

Usage:
  r3d2video [options] <file.r3d | folder> ...
  (no options, e.g. files dropped on r3d2video.exe: it asks)

Options:
  --preset youtube|resolve|lossless   (default: ask, or youtube)
  --layout auto|both|top|bottom|side|split   (default auto: what the recording holds)
  --scale N        integer upscale (default: per preset)
  --fps N          output frame rate (default: the recording's, usually 30)
  --no-eq          the sound as the DSP mixed it, without the console speaker's treble cut
  --out DIR        where to write (default: next to each recording)
  --ffmpeg PATH    ffmpeg to use (default: next to this program, else on PATH)

Needs ffmpeg and r3dconv (tools/r3dconv; next to this program in the Windows zip).
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

LAYOUTS = ["auto", "both", "top", "bottom", "side", "split"]
PRESETS = ["youtube", "resolve", "lossless"]


# ------------------------------------------------------------------ language

def _is_japanese() -> bool:
    if os.environ.get("R3D2VIDEO_LANG"):
        return os.environ["R3D2VIDEO_LANG"].startswith("ja")
    if os.name == "nt":
        try:
            import ctypes
            return (ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3FF) == 0x11
        except Exception:
            return False
    for k in ("LC_ALL", "LC_MESSAGES", "LANG"):
        if os.environ.get(k):
            return os.environ[k].startswith("ja")
    return False


JA = _is_japanese()

T = {
    "drop": ("Drag .r3d files (or a folder) onto r3d2video, or type a path:",
             ".r3d ファイル（またはフォルダ）を r3d2video にドラッグするか、パスを入力してください："),
    "preset_q": ("What for?", "何に使いますか？"),
    "preset_youtube": ("YouTube / sharing (MP4, sharp upscale)", "YouTube・共有用（MP4、くっきり拡大）"),
    "preset_resolve": ("Editing in DaVinci Resolve etc. (DNxHR .mov)", "DaVinci Resolve などで編集（DNxHR .mov）"),
    "preset_lossless": ("Keep exactly (lossless .mkv)", "そのまま保存（完全ロスレス .mkv）"),
    "layout_q": ("Screens?", "画面の並べ方は？"),
    "layout_auto": ("As recorded", "録画のまま"),
    "layout_both": ("Top above bottom", "上下に並べる"),
    "layout_top": ("Top screen only", "上画面だけ"),
    "layout_bottom": ("Bottom screen only", "下画面だけ"),
    "layout_side": ("Side by side", "横に並べる"),
    "layout_split": ("One file per screen", "上下を別ファイルに"),
    "choose": ("Number [1]: ", "番号 [1]: "),
    "no_ffmpeg": ("ffmpeg was not found. Install it once:\n"
                  "  Windows: winget install --id Gyan.FFmpeg -e   (or put ffmpeg.exe next to r3d2video.exe)\n"
                  "  macOS:   brew install ffmpeg\n"
                  "  Linux:   sudo apt install ffmpeg",
                  "ffmpeg が見つかりません。一度だけ入れてください：\n"
                  "  Windows: winget install --id Gyan.FFmpeg -e （または ffmpeg.exe を r3d2video.exe と同じフォルダに置く）\n"
                  "  macOS:   brew install ffmpeg\n"
                  "  Linux:   sudo apt install ffmpeg"),
    "no_r3dconv": ("r3dconv was not found next to this program (build it: see tools/r3dconv).",
                   "r3dconv がこのプログラムと同じフォルダにありません（tools/r3dconv をビルドしてください）。"),
    "no_files": ("No .r3d files found.", ".r3d ファイルが見つかりません。"),
    "converting": ("Converting", "変換中"),
    "done": ("Done", "完了"),
    "failed": ("Failed", "失敗"),
    "no_screen": ("This recording has no {} screen: skipped.", "この録画には{}画面がないため、とばしました。"),
    "top": ("top", "上"),
    "bottom": ("bottom", "下"),
    "summary": ("{} written, {} failed.", "{} 本できました（失敗 {} 本）。"),
    "press_enter": ("Press Enter to close.", "Enter キーで閉じます。"),
}


def t(key: str, *a) -> str:
    s = T[key][1 if JA else 0]
    return s.format(*a) if a else s


# ------------------------------------------------------------------ tools

def _base_dir() -> Path:
    if getattr(sys, "frozen", False):   # PyInstaller exe
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


def find_tool(name: str, explicit: str | None = None) -> str | None:
    if explicit:
        return explicit
    exe = name + (".exe" if os.name == "nt" else "")
    base = _base_dir()
    for cand in (base / exe, base.parent / "build" / exe):
        if cand.is_file():
            return str(cand)
    return shutil.which(name)


# ------------------------------------------------------------------ presets

@dataclass
class Preset:
    name: str
    ext: str
    max_w: int            # auto scale: the largest integer that fits max_w x max_h
    max_h: int
    video: list[str]
    audio: list[str]


PRESET_TABLE = {
    # 1440p tall, at most 3840 wide; x264 for broad compatibility
    "youtube": Preset("youtube", ".mp4", 3840, 1440,
                      ["-c:v", "libx264", "-preset", "slow", "-crf", "16", "-pix_fmt", "yuv420p",
                       "-movflags", "+faststart"],
                      ["-c:a", "aac", "-b:a", "192k", "-ar", "48000"]),
    # fits a 1080p timeline; DNxHR HQ 4:2:2 8-bit (Resolve free reads it), PCM audio
    "resolve": Preset("resolve", ".mov", 1920, 1080,
                      ["-c:v", "dnxhd", "-profile:v", "dnxhr_hq", "-pix_fmt", "yuv422p"],
                      ["-c:a", "pcm_s16le", "-ar", "48000"]),
    # 1x, exact RGB; FLAC keeps the DSP rate
    "lossless": Preset("lossless", ".mkv", 0, 0,
                       ["-c:v", "ffv1", "-level", "3", "-pix_fmt", "bgr0", "-g", "1"],
                       ["-c:a", "flac"]),
}


def auto_scale(p: Preset, w: int, h: int) -> int:
    if not p.max_w:
        return 1
    return max(1, min(p.max_w // w, p.max_h // h))


# ------------------------------------------------------------------ conversion

@dataclass
class Info:
    width: int
    height: int
    fps: int
    seconds: float
    has_audio: bool


def probe(r3dconv: str, path: Path, layout: str | None, wav: Path, no_eq: bool) -> tuple[int, Info | None, str]:
    cmd = [r3dconv, "--info"]
    if no_eq:
        cmd.append("--no-eq")
    if layout:
        cmd += ["--layout", layout]
    cmd += ["--wav", str(wav), str(path)]
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                       encoding="utf-8", errors="replace")
    err = r.stderr
    if r.returncode != 0:
        return r.returncode, None, err
    m = re.match(r"(\d+)x(\d+) (\d+)", err)
    secs = re.search(r"^([\d.]+) s, top", err, re.M)
    audio = re.search(r"^audio: (\d+) frames", err, re.M)
    if not m:
        return 1, None, err
    return 0, Info(int(m[1]), int(m[2]), int(m[3]), float(secs[1]) if secs else 0.0,
                   bool(audio and int(audio[1]) > 0)), err


def convert_one(r3dconv: str, ffmpeg: str, src: Path, out_dir: Path, preset: Preset, layout: str,
                scale: int | None, fps: int | None, no_eq: bool) -> list[Path]:
    """Converts one recording; returns the files written (empty on failure)."""
    layouts = ["top", "bottom"] if layout == "split" else [None if layout == "auto" else layout]
    written: list[Path] = []
    for lay in layouts:
        with tempfile.TemporaryDirectory() as tmp:
            wav = Path(tmp) / "audio.wav"
            rc, info, err = probe(r3dconv, src, lay, wav, no_eq)
            if rc == 3:   # the recording lacks this screen
                print("  " + t("no_screen", t(lay or "top")))
                continue
            if rc != 0 or info is None:
                print(err.strip())
                return []
            s = scale or auto_scale(preset, info.width, info.height)
            rate = fps or info.fps
            suffix = f"_{preset.name}" + (f"_{lay}" if layout == "split" else "")
            dst = out_dir / (src.stem + suffix + preset.ext)

            conv = [r3dconv]
            if no_eq:
                conv.append("--no-eq")
            if lay:
                conv += ["--layout", lay]
            conv += [str(src), str(rate)]

            ff = [ffmpeg, "-hide_banner", "-loglevel", "error", "-stats", "-y",
                  "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{info.width}x{info.height}",
                  "-r", str(rate), "-i", "-"]
            if info.has_audio:
                ff += ["-i", str(wav)]
            vf = []
            if s > 1:
                vf.append(f"scale=iw*{s}:ih*{s}:flags=neighbor")
            if preset.name == "youtube":
                vf.append("setsar=1")
            if vf:
                ff += ["-vf", ",".join(vf)]
            ff += preset.video
            # BT.709 for HD sizes, so players show the same colours as the console
            if preset.name != "lossless":
                ff += ["-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709"]
            ff += (preset.audio + ["-shortest"]) if info.has_audio else ["-an"]
            ff.append(str(dst))

            print(f"  {t('converting')}: {dst.name}  ({info.width * s}x{info.height * s}, {rate} fps)")
            p1 = subprocess.Popen(conv, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            p2 = subprocess.Popen(ff, stdin=p1.stdout)
            p1.stdout.close()   # ffmpeg owns the pipe now
            p2.wait()
            p1.wait()
            if p2.returncode != 0 or p1.returncode not in (0, None):
                print(f"  {t('failed')}: {dst.name}")
                try:
                    dst.unlink()
                except OSError:
                    pass
                return written
            written.append(dst)
    return written


# ------------------------------------------------------------------ interactive

def ask(question: str, options: list[tuple[str, str]]) -> str:
    print()
    print(question)
    for i, (_, label) in enumerate(options, 1):
        print(f"  {i}. {label}")
    while True:
        a = input(t("choose")).strip()
        if a == "":
            return options[0][0]
        if a.isdigit() and 1 <= int(a) <= len(options):
            return options[int(a) - 1][0]


def collect(paths: list[str]) -> list[Path]:
    files: list[Path] = []
    for p in paths:
        q = Path(p.strip().strip('"'))
        if q.is_dir():
            files += sorted(x for x in q.rglob("*") if x.suffix.lower() == ".r3d")
        elif q.is_file():
            files.append(q)
    return files


def main() -> int:
    ap = argparse.ArgumentParser(description="3DS Recorder .r3d -> high-quality video")
    ap.add_argument("inputs", nargs="*")
    ap.add_argument("--preset", choices=PRESETS)
    ap.add_argument("--layout", choices=LAYOUTS)
    ap.add_argument("--scale", type=int)
    ap.add_argument("--fps", type=int)
    ap.add_argument("--no-eq", action="store_true")
    ap.add_argument("--out")
    ap.add_argument("--ffmpeg")
    args = ap.parse_args()

    interactive = sys.stdin.isatty() and (args.preset is None or not args.inputs)
    pause = interactive and getattr(sys, "frozen", False) and os.name == "nt"   # a window from a drop

    try:
        ffmpeg = find_tool("ffmpeg", args.ffmpeg)
        r3dconv = find_tool("r3dconv")
        if not ffmpeg:
            print(t("no_ffmpeg"))
            return 1
        if not r3dconv:
            print(t("no_r3dconv"))
            return 1

        inputs = list(args.inputs)
        if not inputs and interactive:
            print(t("drop"))
            inputs = [input("> ")]
        files = collect(inputs)
        if not files:
            print(t("no_files"))
            return 1

        preset = args.preset
        layout = args.layout
        if interactive and preset is None:
            preset = ask(t("preset_q"), [(p, t("preset_" + p)) for p in PRESETS])
            if layout is None:
                layout = ask(t("layout_q"), [(l, t("layout_" + l)) for l in LAYOUTS])
        preset = preset or "youtube"
        layout = layout or "auto"
        if args.scale is not None and args.scale < 1:
            ap.error("--scale must be 1 or more")

        ok = bad = 0
        for f in files:
            print(f"\n{f}")
            out_dir = Path(args.out) if args.out else f.parent
            out_dir.mkdir(parents=True, exist_ok=True)
            done = convert_one(r3dconv, ffmpeg, f, out_dir, PRESET_TABLE[preset], layout,
                               args.scale, args.fps, args.no_eq)
            if done:
                ok += len(done)
                for d in done:
                    print(f"  {t('done')}: {d}")
            else:
                bad += 1
        print()
        print(t("summary", ok, bad))
        return 0 if bad == 0 else 1
    finally:
        if pause:
            try:
                input(t("press_enter"))
            except EOFError:
                pass


if __name__ == "__main__":
    sys.exit(main())
