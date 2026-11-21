# r3d2video — recordings to high-quality video on a computer

The console's own MP4 is made for a phone: small (400 × 480) and compressed. There are two ways to
make a better video from a recording (`.r3d`, in `/3ds/recorder/recordings/` on the SD card):

| | [The converter page](https://bolgarmaxym97.github.io/3ds-recorder/) | r3d2video (this page) |
|---|---|---|
| Install | nothing: Chrome, Edge or Safari on a computer | Windows zip, or Python + ffmpeg |
| Size | 1×, 2×, 3× (sharp pixels) | any whole number (default per preset) |
| Screens | as recorded, top, bottom, side by side | the same, plus one file per screen |
| For | sharing, YouTube (higher bitrate), editing (every frame a keyframe) | YouTube (H.264 CRF 16), DaVinci Resolve (DNxHR HQ), lossless (FFV1 + FLAC) |

**Start with the page**: drop the `.r3d` on it, choose the size, screens and what the video is for.
The file never leaves the computer. Use r3d2video when you need DNxHR, a lossless file or one file
per screen.

## Windows

1. Install ffmpeg once: `winget install --id Gyan.FFmpeg -e`
   (or put `ffmpeg.exe` next to `r3d2video.exe`).
2. Download [`r3d2video-windows.zip`](https://github.com/BolgarMaxym97/3ds-recorder/releases/latest/download/r3d2video-windows.zip) (with every release from 1.6.0) and unzip it anywhere.
3. Drag one or more `.r3d` files (or a folder) onto **r3d2video.exe**, then choose what for and
   how to lay out the screens. The videos are written next to the recordings.

## macOS / Linux

Needs Python 3, a C compiler and ffmpeg. Build `r3dconv` once (`bash tests/pcconv/run.sh` does
it and tests everything), then:

    python3 tools/r3d2video.py --preset youtube recording.r3d

## Presets

| Preset | File | Video | Sound | Size (both screens) |
|---|---|---|---|---|
| **youtube** | `.mp4` | H.264, CRF 16 | AAC 192 kb/s, 48 kHz | 1200 × 1440 (3×) |
| **resolve** | `.mov` | DNxHR HQ (DaVinci Resolve, free version too) | PCM, 48 kHz | 800 × 960 (2×, fits 1080p) |
| **lossless** | `.mkv` | FFV1 RGB, every pixel as recorded | FLAC, as recorded | 400 × 480 (1×) |

Upscaling is by whole numbers with nearest neighbour, so pixels stay sharp. It happens **before**
the colour is subsampled (4:2:0 / 4:2:2), so thin coloured lines and text do not bleed: at 1× a
4:2:0 MP4 shares one colour between every 2 × 2 pixels of the 3DS screen. YouTube also gives
uploads of 1440p and above a higher bitrate.

## Layouts

`both` top above bottom (as recorded) · `top` · `bottom` (320 × 240, its own size) · `side` side
by side · `split` one file per screen · `auto` what the recording holds (default).

## Options

    --preset youtube|resolve|lossless
    --layout auto|both|top|bottom|side|split
    --scale N        whole-number upscale instead of the preset's
    --fps N          output frame rate (default: the recording's)
    --no-eq          the sound as the DSP mixed it (no treble cut like the console's speaker)
    --out DIR        write the videos there
    --ffmpeg PATH    a specific ffmpeg

---

r3d2video was written by [noctis1008-wq](https://github.com/noctis1008-wq) in their fork of this
project and taken into it for 1.6.0. Thank you!
