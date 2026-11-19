# Building from source

[← README](../README.md)

The recorder is a Luma3DS sysmodule (`sysmod/`) plus the control app (`app/`), which carries the sysmodule and the
launcher plugin (`plugin/`) inside. A Docker image with devkitARM, libctru and 3gxtool builds all three:

```sh
docker build -t 3dsrec-dev tools/
tools/dkp.sh plugin make          # plugin/rec3ds-launch.3gx
tools/dkp.sh sysmod make          # sysmod/rec3ds.cxi
tools/dkp.sh app make cia         # app/3ds-recorder.cia and .3dsx
```

| Folder | What |
|---|---|
| `sysmod/` | the recorder: reads the screens and the sound, compresses them (XR32, lossless; C420 for pictures in motion) and writes `.r3d` |
| `app/` | the app: install, settings, gallery, player, the Wi-Fi server for the phone |
| `plugin/` | the launcher plugin for extended-memory games on an Old 3DS (starts the recorder again inside the game) |
| `prep/` | turns an `.r3d` into MP4 (H.264 + AAC) on the console; makes Replay clips into `.r3d` recordings; joins the parts of a recording the recorder paused |
| `web/` | the page the phone opens; also the `.r3d` converter for computers (GitHub Pages, `tools/build-web.py --local`) |
| `codec/` | the screen codecs and the `.r3d` format |
| `tools/` | build helpers, `r3dconv` / `r3d2mp4.sh` (`.r3d` → MP4 on a computer), Azahar helpers (`tools/azahar/`) |
| `tests/` | host tests, on-device tests and benchmarks |

**Screenshots** for the README (`docs/screenshots/`) of screens that do not need the recorder running (settings,
help, language) come from Azahar: `tools/azahar/azshot.sh capture.png`, then `tools/azahar/az3ds.sh capture.png
out.png` makes the 400 × 480 picture.
