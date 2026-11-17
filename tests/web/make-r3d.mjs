// Writes a synthetic .r3d (codec/include/r3d_format.h): raw R3DF frames with a moving bar and a
// 440 Hz tone, for browser tests when no real recording is at hand. Check it with compare.mjs first.
// Usage: node tests/web/make-r3d.mjs <out.r3d> [--mode both|top|bottom] [--secs 8] [--fps 15] [--no-audio]
import { writeFileSync } from 'node:fs';

const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : d; };
const out = argv[0];
const mode = { top: 3, both: 4, bottom: 5 }[opt('--mode', 'both')];
const secs = Number(opt('--secs', 8));
const fps = Number(opt('--fps', 15));
const audio = !argv.includes('--no-audio');
const RATE_MHZ = 32728498;
const M = { FILE: 0x42443352, FRAME: 0x46443352, AUDIO: 0x41443352, END: 0x45443352 };
const KEY = 0x40;

const parts = [];
const head = Buffer.alloc(32);
head.writeUInt32LE(M.FILE, 0);
head.writeUInt32LE(6, 4);
head.writeUInt8(0, 16);
head.writeUInt8(mode, 17);
head.writeUInt8(fps, 18);
head.writeUInt32LE(audio ? RATE_MHZ : 0, 20);
parts.push(head);

function rec(magic, payload, timeUs, screen, cols) {
  const h = Buffer.alloc(16);
  h.writeUInt32LE(magic, 0);
  h.writeUInt32LE(payload.length, 4);
  h.writeUInt32LE(timeUs >>> 0, 8);
  h.writeUInt8(screen, 12);
  h.writeUInt16LE(cols, 14);
  parts.push(h, payload);
}

// column-major RGB565, word 0 of a column = bottom row
function picture(cols, n, hue) {
  const px = Buffer.alloc(cols * 240 * 2);
  const bar = (n * 7) % cols;
  for (let x = 0; x < cols; x++) {
    for (let y = 0; y < 240; y++) {
      const on = Math.abs(x - bar) < 12;
      const r = on ? 31 : (x * 31 / cols) | 0, g = on ? 63 : (y * 63 / 240) | 0, b = on ? 31 : hue;
      px.writeUInt16LE((r << 11) | (g << 5) | b, (x * 240 + y) * 2);
    }
  }
  return px;
}

const rateHz = RATE_MHZ / 1000;
let audioFrame = 0;
const frames = secs * fps;
for (let n = 0; n < frames; n++) {
  const timeUs = Math.round((n * 1e6) / fps);
  // the tone up to this picture's time, 160 samples per record
  while (audio && (audioFrame * 160 * 1e6) / rateHz < timeUs) {
    const pcm = Buffer.alloc(640);
    for (let i = 0; i < 160; i++) {
      const v = Math.round(8000 * Math.sin((2 * Math.PI * 440 * (audioFrame * 160 + i)) / rateHz));
      pcm.writeInt16LE(v, i * 4);
      pcm.writeInt16LE(v, i * 4 + 2);
    }
    rec(M.AUDIO, pcm, Math.round((audioFrame * 160 * 1e6) / rateHz), 0, 160);
    audioFrame++;
  }
  if (mode !== 5) rec(M.FRAME, picture(400, n, 8), timeUs, 0 | KEY, 400);
  if (mode !== 3) rec(M.FRAME, picture(320, n + 20, 24), timeUs + 1000, 1 | KEY, 320);
}
const end = Buffer.alloc(16);
end.writeUInt32LE(mode !== 5 ? frames : 0, 0);
end.writeUInt32LE(mode !== 3 ? frames : 0, 4);
rec(M.END, end, Math.round(secs * 1e6), 0, 0);
writeFileSync(out, Buffer.concat(parts));
console.log(`${out}: ${secs} s, mode ${mode}, ${fps} fps, audio ${audio}`);
