// Drives the phone page in headless Chrome (DevTools protocol) and saves the video it makes into
// build/web-result.<ext> (through tests/web/serve.py's test-only upload), for ffprobe.
// Usage: node tests/web/run.mjs <page url> [--live]   (--live: presses Start, the MediaRecorder path)
import { spawn } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const url = process.argv[2];
const live = process.argv.includes('--live');
const chrome = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const port = 9333;
const proc = spawn(chrome, ['--headless=new', `--remote-debugging-port=${port}`, `--user-data-dir=${mkdtempSync(join(tmpdir(), 'cdp'))}`,
  '--autoplay-policy=no-user-gesture-required', '--no-first-run', 'about:blank'], { stdio: 'ignore' });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let ws;
for (let i = 0; i < 50 && !ws; i++) {
  await sleep(200);
  try {
    const list = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
    const page = list.find((t) => t.type === 'page');
    if (page) ws = new WebSocket(page.webSocketDebuggerUrl);
  } catch (e) { /* not up yet */ }
}
await new Promise((r) => ws.addEventListener('open', r));
let id = 0;
const pending = new Map();
ws.addEventListener('message', (m) => {
  const d = JSON.parse(m.data);
  if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); }
  if (d.method === 'Runtime.exceptionThrown') console.log('exception:', d.params.exceptionDetails.exception?.description || d.params.exceptionDetails.text);
  if (d.method === 'Runtime.consoleAPICalled') console.log('console:', d.params.args.map((a) => a.value ?? a.description).join(' '));
});
const send = (method, params = {}) => new Promise((r) => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const ev = async (expr) => (await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
await send('Runtime.enable');
const shots = process.argv.includes('--shot'); // phone-sized screenshots: build/web-page-*.png
if (shots) await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
const shot = async (tag) => {
  if (!shots) return;
  const r = await send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true });
  (await import('node:fs')).writeFileSync(join('build', `web-page-${tag}.png`), Buffer.from(r.result.data, 'base64'));
};
await send('Page.navigate', { url });
await sleep(1500);
console.log('secure context:', await ev('isSecureContext'), '| VideoEncoder:', await ev('typeof VideoEncoder'));
await shot('start');
if (live) await ev("document.getElementById('start').hidden ? 'no start button' : (document.getElementById('start').click(), 'started')").then(console.log);
const t0 = Date.now();
let last = '';
for (;;) {
  await sleep(2000);
  const st = await ev(`JSON.stringify({p1: document.getElementById('p1').textContent, s1: document.getElementById('s1x').textContent,
    p2: document.getElementById('p2').textContent, s2: document.getElementById('s2x').textContent,
    err: document.getElementById('error').hidden ? '' : document.getElementById('error').textContent,
    done: !document.getElementById('result').hidden})`);
  const s = JSON.parse(st);
  const line = `${((Date.now() - t0) / 1000).toFixed(0)} s: ${s.p1} | ${s.s1} | video ${s.p2} (${s.s2})`;
  if (line !== last) console.log(line);
  if (Date.now() - t0 > 4000 && Date.now() - t0 < 6100) await shot('progress');
  last = line;
  if (s.err) { console.log('ERROR:', s.err); break; }
  if (s.done) {
    const saved = await ev(`(async () => { const a = document.getElementById('save'); const b = await (await fetch(a.href)).blob();
      const r = await fetch('/upload.' + a.download.split('.').pop(), { method: 'POST', body: b });
      return (await r.text()) + ' ' + b.size + ' ' + a.download; })()`);
    console.log('saved:', saved, `after ${((Date.now() - t0) / 1000).toFixed(1)} s`);
    await sleep(500);
    await shot('done');
    break;
  }
}
proc.kill();
process.exit(0);
