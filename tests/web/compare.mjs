// Checks web/r3d.js against tools/r3dconv on a real recording: the same video frames (rgb24) and the
// same audio samples, byte for byte. The file is fed in random piece sizes, as the network delivers it.
// Usage: node tests/web/compare.mjs <file.r3d> [--no-eq] [--inputs]   (--no-eq: the sound without the treble cut, both sides)   (builds build/r3dconv first: tools/r3d2mp4.sh does)
// --inputs: the buttons drawn over the video on both sides (codec/source/inputs.c and web/inputs.js): the
// frames then agree within 2 per colour (float and double), checked frame by frame
import { createHash } from 'node:crypto';
import { execFileSync, spawn } from 'node:child_process';
import { readFileSync, mkdtempSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const repo = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const R3D = createRequire(import.meta.url)(join(repo, 'web', 'r3d.js'));
const R3DInputs = createRequire(import.meta.url)(join(repo, 'web', 'inputs.js'));
const file = process.argv[2];
const noEq = process.argv.includes('--no-eq');
const withInputs = process.argv.includes('--inputs');
const conv = join(repo, 'build', 'r3dconv');
if (!existsSync(conv))
  execFileSync('cc', ['-std=c11', '-O2', '-Wall', '-I' + join(repo, 'codec/include'), '-o', conv,
    join(repo, 'tools/r3dconv/r3dconv.c'), join(repo, 'codec/source/codec.c'), join(repo, 'codec/source/huff.c'),
    join(repo, 'codec/source/inputs.c'), '-lm']);

// r3dconv: video on stdout, audio into a wav
const wav = join(mkdtempSync(join(tmpdir(), 'r3dcmp')), 'a.wav');
const refVideo = createHash('md5');
let refBytes = 0;
const refRaw = [];
await new Promise((ok, fail) => {
  const p = spawn(conv, [...(noEq ? ['--no-eq'] : []), ...(withInputs ? ['--inputs'] : []), '--wav', wav, file], { stdio: ['ignore', 'pipe', 'ignore'] });
  p.stdout.on('data', (d) => { refVideo.update(d); refBytes += d.length; if (withInputs) refRaw.push(d); });
  p.on('close', (c) => (c === 0 ? ok() : fail(new Error('r3dconv ' + c))));
});
const refAudio = createHash('md5').update(readFileSync(wav).subarray(44)).digest('hex');

// web/r3d.js
const vid = createHash('md5'), aud = createHash('md5');
let w = 0, h = 0, frames = 0, samples = 0, fps = 30, inLayout = null, worst = 0;
const ref = withInputs ? Buffer.concat(refRaw) : null;
const t0 = Date.now();
const c = new R3D.Converter({
  onHeader: (hd) => { w = hd.width; h = hd.height; fps = hd.fps; inLayout = R3DInputs.layout(R3DInputs.screensFor(hd, 'auto')); },
  onFrame: (rgba, i) => {
    if (withInputs) {
      rgba = new Uint8ClampedArray(rgba);
      R3DInputs.drawRgba(rgba, w, h, inLayout, c.inputs.at(Math.floor((i * 1e6) / fps)));
    }
    const rgb = Buffer.alloc(w * h * 3);
    for (let i = 0, o = 0; i < rgba.length; i += 4, o += 3) { rgb[o] = rgba[i]; rgb[o + 1] = rgba[i + 1]; rgb[o + 2] = rgba[i + 2]; }
    if (ref) {
      const r = ref.subarray(frames * rgb.length, (frames + 1) * rgb.length);
      for (let k = 0; k < r.length; k++) { const d = Math.abs(r[k] - rgb[k]); if (d > worst) worst = d; }
    }
    vid.update(rgb);
    frames++;
  },
  onAudio: (s) => { aud.update(Buffer.from(s.buffer, s.byteOffset, s.byteLength)); samples += s.length / 2; },
}, { eq: !noEq });
const data = readFileSync(file);
let seed = 12345;
for (let p = 0; p < data.length;) {
  seed = (seed * 1103515245 + 12345) & 0x7fffffff;
  const n = 1 + (seed % (3 << 20));
  c.push(data.subarray(p, p + n));
  p += n;
}
c.finish();
const secs = (Date.now() - t0) / 1000;

const v = vid.digest('hex'), a = aud.digest('hex');
console.log(`r3dconv: ${refBytes / (w * h * 3)} frames, video ${refVideo.digest('hex')}, audio ${refAudio}`);
console.log(`r3d.js:  ${frames} frames, video ${v}, audio ${a} (${samples} samples, ${secs.toFixed(1)} s)`);
if (withInputs) console.log(`buttons drawn: the largest difference per colour ${worst}`);
if (withInputs && worst > 2) { console.log('FAIL: the buttons differ'); process.exit(1); }
