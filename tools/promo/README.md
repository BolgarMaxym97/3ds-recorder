# Promo video

A ~47 s 1920×1080 promo made with [Remotion](https://www.remotion.dev) and React Three Fiber: an Old 3DS
and an iPhone model on black, the console's own recordings on its screens, captions, logo at the end.

```bash
cd tools/promo
npm install
# 1. MP4s from the console's .r3d files (tools/r3d2mp4.sh) into build/promo/mp4, then cut the clips:
bash prep.sh                  # -> public/clips/*.mp4 (ffmpeg 4x), public/music.m4a, public/models
bash upscale.sh               # optional: Real-ESRGAN 4x instead, cached in build/promo/upscale (slow once)
npm run studio                # preview and tweak in the browser
npm run render                # -> out/promo.mp4
```

- `src/timeline.ts` — shots: clip, camera move, caption. Clip names and cut points are in `prep.sh`.
- `src/Real3DS.tsx` — the console model with a working hinge (sizes in cm); screens map the 400×480
  recording (top 400×240 over the bottom 320×240). `src/IPhone.tsx` — the phone.
- `src/ModelTest.tsx` — scratch composition for checking model placement.
- Models (put the glTF folders into build/promo/models): "Nintendo 3DS" by Broeach8972 and
  "Apple iPhone 13 Pro Max" by DatSketch, Sketchfab, CC BY 4.0 — credit them wherever the video is posted.
- Music: "High Energy Adventure Theme" by srg774, CC0, from OpenGameArt.
- `public/` and `out/` are not committed; the rendered video goes to a GitHub release.
