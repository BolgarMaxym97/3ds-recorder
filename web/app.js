/*
 * The phone page for one recording (?f=<name>). The console makes an MP4 of it (app/source/prepare.c):
 * the page shows how far that is, downloads the MP4 in pieces and offers it to save. Where the console
 * could not make one, the page downloads the recording itself, turns it into a video in the browser
 * and offers that:
 *   fast  WebCodecs H.264 + AAC -> MP4 (mp4-muxer): Safari and every browser on iOS, also on http
 *   live  MediaRecorder on a canvas + Web Audio, in real time (Chromium and Firefox give WebCodecs
 *         only to https pages; the console serves http): MP4 where the browser writes it, else WebM
 *   none  only the original .r3d
 * The download runs ahead of the conversion; both pause for each other, so memory stays small.
 */
(function () {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const TEXT = {
    en: {
      length: 'Length', size: 'Size', eta: 'Transfer', s1: 'Download from the console', s2: 'Make the video',
      s3: 'Save', waiting: 'waiting', fast: 'fast: the phone\'s video encoder',
      live: 'in real time: keep this page open and the screen on', none: 'this browser cannot make videos',
      start: 'Start', startHint: 'Tap Start: the browser lets a page record sound only after a tap.',
      keep: 'Keep this page open and the screen on until the video is ready. If the page is hidden, the video waits.',
      long: 'A long recording: about %1 over the 3DS Wi-Fi. For long ones the SD card is faster.',
      save: 'Save the video', share: 'Share / save to Photos', raw: 'or the original .r3d, for a computer',
      sd: 'Recordings can also be taken from the SD card: turn the console off and put the card into a computer, they are in /3ds/recorder/recordings.',
      howIos: 'Tap “Save the video”, then open Files → Downloads → the video → Share → Save Video: it goes to Photos.',
      howAndroid: 'Tap “Save the video”: it goes to Downloads and shows up in the Gallery.',
      howOther: 'Tap “Save the video”: it goes to your downloads folder.',
      ready: 'ready', retry: 'Try again', lost: 'The connection to the console was lost. Is the console still on the “Download” screen, lid open?',
      notFound: 'This recording is not offered any more. Pick it on the console again (Gallery → Y).',
      noFile: 'No recording in the link. Scan the code on the console again.',
      failed: 'Could not make the video (%1). The original .r3d is below.', paused: 'paused: waiting for the download',
      hidden: 'paused: the page is hidden', min: 'min', sec: 's', left: '%1 left', mbs: 'MB/s',
      c1: 'Converting on the console', c1x: 'converting into a small MP4: the 3DS Wi-Fi sends it quickly', c1q: 'waiting its turn',
      c2: 'Download to the phone', direct: 'Download directly', directLink: 'or download directly',
      big: 'A long video (%1): the phone\'s browser downloads it by itself, look in its downloads. Keep the console on the “Download” screen, lid open.',
      diag: 'Log for the developer (.txt)',
    },
    uk: {
      length: 'Тривалість', size: 'Розмір', eta: 'Передача', s1: 'Завантаження з консолі', s2: 'Робимо відео',
      s3: 'Зберегти', waiting: 'чекає', fast: 'швидко: відеокодер телефона',
      live: 'у реальному часі: тримайте сторінку відкритою, а екран увімкненим', none: 'цей браузер не вміє робити відео',
      start: 'Почати', startHint: 'Натисніть «Почати»: браузер дозволяє сторінці записувати звук лише після дотику.',
      keep: 'Тримайте сторінку відкритою, а екран увімкненим, доки відео не буде готове. Якщо сторінку сховати, відео чекає.',
      long: 'Довгий запис: ≈ %1 через Wi-Fi 3DS. Довгі записи швидше забрати з SD-картки.',
      save: 'Зберегти відео', share: 'Поділитися / зберегти у Фото', raw: 'або оригінал .r3d — для комп\'ютера',
      sd: 'Записи можна забрати й через SD-картку: вимкніть консоль і вставте картку в комп\'ютер — вони в /3ds/recorder/recordings.',
      howIos: 'Натисніть «Зберегти відео», потім Файли → Завантаження → відео → Поділитися → Зберегти відео: воно потрапить у Фото.',
      howAndroid: 'Натисніть «Зберегти відео»: воно потрапить у «Завантаження» і з\'явиться в Галереї.',
      howOther: 'Натисніть «Зберегти відео»: воно потрапить у теку завантажень.',
      ready: 'готово', retry: 'Спробувати ще раз', lost: 'Зв\'язок з консоллю втрачено. Консоль досі на екрані «Завантажити», кришка відкрита?',
      notFound: 'Цей запис уже не пропонується. Виберіть його на консолі ще раз (Галерея → Y).',
      noFile: 'У посиланні немає запису. Відскануйте код на консолі ще раз.',
      failed: 'Не вдалося зробити відео (%1). Нижче — оригінал .r3d.', paused: 'пауза: чекаємо завантаження',
      hidden: 'пауза: сторінку сховано', min: 'хв', sec: 'с', left: 'ще %1', mbs: 'MB/с',
      c1: 'Конвертація на консолі', c1x: 'конвертую в невелике MP4: Wi-Fi 3DS передасть його швидко', c1q: 'чекає черги',
      c2: 'Завантаження на телефон', direct: 'Завантажити напряму', directLink: 'або завантажити напряму',
      big: 'Довге відео (%1): браузер телефона сам його завантажить — дивіться в його завантаженнях. Тримайте консоль на екрані «Завантажити» з відкритою кришкою.',
      diag: 'Журнал для розробника (.txt)',
    },
  };
  let L = TEXT.en;
  const t = (k, ...a) => a.reduce((s, v, i) => s.replace('%' + (i + 1), v), L[k] || TEXT.en[k] || k);

  const CHUNK = 512 << 10;      // bytes per request (~1-2 s each on the 3DS Wi-Fi: progress moves)
  const PARALLEL = 3;           // requests at once (+15-25% on the 3DS Wi-Fi)
  const STALL_MS = 20000;       // a request with no byte for this long is asked again (weak signal)
  const BIG_MP4 = 150 << 20;    // larger MP4s are not put together in the page (phone memory)
  const FEED = 128 << 10;       // bytes handed to the decoder at a time (bounds the frames in flight)
  const WIFI_KBS = 420;         // for the first estimate (O3DS measured 350-490 KB/s)
  const AAC_RATE = 48000;
  const ua = navigator.userAgent;
  const isIOS = /iPad|iPhone|iPod/.test(ua) || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
  const isAndroid = /Android/.test(ua);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  // the QR code names the recording; the address typed by hand (shown under the code) does not:
  // then it is the one the console offers
  let name = new URLSearchParams(location.search).get('f');
  let info = null;

  // One line into the console's app.log: what the phone did, so a test far away needs no
  // questions afterwards (the "Log for the developer" file has it). Never fails the page.
  let reports = 0;
  function report(msg) {
    if (++reports > 200) return;
    try {
      fetch('/api/log?m=' + encodeURIComponent(String(msg).slice(0, 300)), { cache: 'no-store' }).catch(() => {});
    } catch (e) { /* no fetch */ }
  }
  const why = (e) => (e && (e.name ? e.name + ': ' : '') + (e.message || '')) || String(e);
  window.addEventListener('error', (e) => report('js error: ' + e.message + ' (' + (e.filename || '').split('/').pop() + ':' + e.lineno + ')'));
  window.addEventListener('unhandledrejection', (e) => report('js promise: ' + why(e.reason)));
  document.addEventListener('visibilitychange', () => report('page ' + document.visibilityState));

  /* ------------------------------------------------------------ UI */

  function clock(sec) {
    sec = Math.max(0, Math.round(sec));
    const h = Math.floor(sec / 3600), m = Math.floor(sec / 60) % 60, s = sec % 60;
    return (h ? h + ':' + String(m).padStart(2, '0') : String(m).padStart(2, '0')) + ':' + String(s).padStart(2, '0');
  }
  function mb(bytes) { return bytes >= 1 << 30 ? (bytes / (1 << 30)).toFixed(1) + ' GB' : Math.round(bytes / (1 << 20)) + ' MB'; }
  function duration(sec) { return sec >= 90 ? '≈ ' + Math.round(sec / 60) + ' ' + t('min') : '≈ ' + Math.round(sec) + ' ' + t('sec'); }
  function when(n) { // yyyymmdd_hhmmss.r3d
    const m = /^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})/.exec(n || '');
    if (!m) return n || '';
    const d = new Date(+m[1], m[2] - 1, +m[3], +m[4], +m[5]);
    return d.toLocaleDateString(L === TEXT.uk ? 'uk-UA' : 'en-GB', { day: 'numeric', month: 'long', year: 'numeric' }) +
      ' · ' + m[4] + ':' + m[5];
  }
  function step(i, state, pct, text, extra) {
    const el = $('s' + i);
    el.classList.toggle('on', state === 'on');
    el.classList.toggle('done', state === 'done');
    if (pct !== undefined && $('b' + i)) $('b' + i).style.width = Math.min(100, pct) + '%';
    if (text !== undefined && $('p' + i)) $('p' + i).textContent = text;
    if (extra !== undefined) $('s' + i + 'x').textContent = extra;
  }
  function show(id, text) { const el = $(id); if (text !== undefined) el.textContent = text; el.hidden = false; }
  function fail(text, retry) {
    report('error shown: ' + text.slice(0, 80));
    show('error', text);
    if (retry) {
      const b = $('retry');
      b.textContent = t('retry');
      b.hidden = false;
      b.onclick = () => { b.hidden = true; $('error').hidden = true; retry(); };
    }
  }

  /* ------------------------------------------------------------ download */

  // Pieces of the file in parallel, handed on in order. feed(u8) may wait (the consumer is behind).
  class Download {
    constructor(url, size, feed, progress) {
      this.url = url; this.size = size; this.feed = feed; this.progress = progress;
      this.next = 0;          // next offset to request
      this.fed = 0;           // handed on up to here
      this.got = 0;           // bytes received
      this.ready = new Map(); // offset -> data, received out of order
      this.redo = [];         // offsets whose request failed
      this.t0 = performance.now();
      this.feeding = Promise.resolve();
    }
    async run() {
      this.error = null;
      const workers = [];
      for (let i = 0; i < PARALLEL; i++) workers.push(this.worker());
      await Promise.all(workers);
      await this.feeding;
      if (this.error) throw this.error;
    }
    async worker() {
      while (!this.error) {
        let off;
        if (this.redo.length) off = this.redo.shift();
        else if (this.next < this.size) { off = this.next; this.next += CHUNK; }
        else return;
        // not too far ahead of what has been handed on
        while (!this.error && off - this.fed > CHUNK * (PARALLEL + 3)) await sleep(50);
        try {
          const data = await this.fetch(off, Math.min(off + CHUNK, this.size) - 1);
          this.ready.set(off, data);
          this.drain();
        } catch (e) {
          this.redo.push(off);
          this.error = e;
        }
      }
    }
    // Bytes a..b. With a weak signal a request can stall: it is given up after STALL_MS without
    // a single byte (not after a fixed time: slow is fine) and asked again from where it stopped.
    async fetch(a, b) {
      const out = new Uint8Array(b - a + 1);
      let have = 0, lastErr, fails = 0;
      while (have < out.length) {
        const ctl = new AbortController();
        let timer = setTimeout(() => ctl.abort(), STALL_MS);
        const moved = () => { clearTimeout(timer); timer = setTimeout(() => ctl.abort(), STALL_MS); };
        const before = have;
        try {
          const r = await fetch(this.url, { headers: { Range: `bytes=${a + have}-${b}` }, signal: ctl.signal, cache: 'no-store' });
          if (r.status === 404) { const e = new Error('404'); e.notFound = true; throw e; }
          if (r.status !== 206 && r.status !== 200) throw new Error('HTTP ' + r.status);
          if (r.body && r.body.getReader) {
            const rd = r.body.getReader();
            for (;;) {
              const { done, value } = await rd.read();
              if (done) break;
              moved();
              if (have + value.length > out.length) throw new Error('long read');
              out.set(value, have);
              have += value.length;
              this.count(value.length);
            }
          } else {
            const buf = new Uint8Array(await r.arrayBuffer());
            if (have + buf.length > out.length) throw new Error('long read');
            out.set(buf, have);
            have += buf.length;
            this.count(buf.length);
          }
          if (have < out.length) throw new Error('short read');
        } catch (e) {
          if (e.notFound) throw e;
          lastErr = e;
          fails = have > before ? 0 : fails + 1; // progress resets the count: only a dead link gives up
          report(`retry ${this.url.split('/').pop()} @${a + have} (${have - before} B moved): ${why(e)}, fail ${fails}`);
          if (fails >= 6) throw lastErr;
          await sleep(1000 * fails); // the console may be busy (503) or the signal weak
        } finally {
          clearTimeout(timer);
        }
      }
      return out;
    }
    count(n) {
      this.got += n;
      this.progress(this.got, this.size, this.got / ((performance.now() - this.t0) / 1000));
    }
    drain() {
      this.feeding = this.feeding.then(async () => {
        while (this.ready.has(this.fed)) {
          const d = this.ready.get(this.fed);
          this.ready.delete(this.fed);
          for (let p = 0; p < d.length; p += FEED) await this.feed(d.subarray(p, p + FEED));
          this.fed += d.length;
        }
      });
    }
  }

  /* ------------------------------------------------------------ fast: WebCodecs */

  // Safari (iOS 18) gives the AAC decoderConfig.description as a whole ES_Descriptor (tag 0x03)
  // instead of the 2-byte AudioSpecificConfig; the muxer then writes a broken esds (no sound).
  // Take the DecoderSpecificInfo (tag 0x05) out of it (tests/webcodecs, iPhone 2026-11-03).
  function asc(desc) {
    const b = desc instanceof ArrayBuffer ? new Uint8Array(desc) : new Uint8Array(desc.buffer, desc.byteOffset, desc.byteLength);
    if (b[0] !== 0x03) return b;
    const len = (i) => { let n = 0, k = 0; do { n = (n << 7) | (b[i + k] & 0x7f); } while (b[i + k++] & 0x80 && k < 4); return [n, k]; };
    const find = (i, end, tag) => {
      while (i < end) {
        const tg = b[i], [n, k] = len(i + 1), body = i + 1 + k;
        if (tg === tag) return b.slice(body, body + n);
        if (tg === 0x03) {
          const flags = b[body + 2]; let j = body + 3;
          if (flags & 0x80) j += 2; if (flags & 0x40) j += 1 + b[j]; if (flags & 0x20) j += 2;
          const r = find(j, body + n, tag); if (r) return r;
        }
        if (tg === 0x04) { const r = find(body + 13, body + n, tag); if (r) return r; }
        i = body + n;
      }
      return null;
    };
    return find(0, b.length, 0x05) || b;
  }

  async function fastSupport(height, fps, audio) {
    if (typeof VideoEncoder === 'undefined' || typeof VideoFrame === 'undefined') return null;
    if (audio && (typeof AudioEncoder === 'undefined' || typeof AudioData === 'undefined')) return null;
    let codec = null;
    for (const c of ['avc1.4d401f', 'avc1.42e01f', 'avc1.640028']) {
      try {
        const r = await VideoEncoder.isConfigSupported({ codec: c, width: 400, height, bitrate: 2500000, framerate: fps });
        if (r.supported) { codec = c; break; }
      } catch (e) { /* next */ }
    }
    if (!codec) return null;
    if (audio) {
      try {
        const r = await AudioEncoder.isConfigSupported({ codec: 'mp4a.40.2', sampleRate: AAC_RATE, numberOfChannels: 2, bitrate: 160000 });
        if (!r.supported) return null;
      } catch (e) { return null; }
    }
    return codec;
  }

  // Streaming resampler, stereo s16 at `from` Hz -> stereo s16 at `to` Hz (Catmull-Rom).
  class Resampler {
    constructor(from, to) { this.step = from / to; this.pos = 0; this.hist = new Float32Array(8); this.have = 0; }
    run(inp) { // Int16Array stereo -> Int16Array stereo
      const n = inp.length / 2;
      const src = new Float32Array((n + 4) * 2); // 3 frames of history, then the new ones
      src.set(this.hist.subarray(0, 6));
      for (let i = 0; i < inp.length; i++) src[6 + i] = inp[i];
      const total = n + 3;
      const out = [];
      let p = this.pos;
      while (p + 2 < total) {
        const i = Math.floor(p), f = p - i;
        for (let ch = 0; ch < 2; ch++) {
          const y0 = src[2 * i + ch], y1 = src[2 * (i + 1) + ch], y2 = src[2 * (i + 2) + ch], y3 = src[2 * (i + 3) + ch];
          const v = y1 + 0.5 * f * (y2 - y0 + f * (2 * y0 - 5 * y1 + 4 * y2 - y3 + f * (3 * (y1 - y2) + y3 - y0)));
          out.push(v > 32767 ? 32767 : v < -32768 ? -32768 : Math.round(v));
        }
        p += this.step;
      }
      this.hist.set(src.subarray(2 * n, 2 * n + 6));
      this.pos = p - n;
      return Int16Array.from(out);
    }
  }

  function fastConvert(hdr, codec) {
    const target = new Mp4Muxer.ArrayBufferTarget();
    const muxer = new Mp4Muxer.Muxer({
      target,
      video: { codec: 'avc', width: 400, height: hdr.height, frameRate: hdr.fps },
      audio: hdr.audio ? { codec: 'aac', numberOfChannels: 2, sampleRate: AAC_RATE } : undefined,
      fastStart: 'in-memory',
      firstTimestampBehavior: 'offset',
    });
    let error = null;
    const venc = new VideoEncoder({ output: (c, m) => muxer.addVideoChunk(c, m), error: (e) => { error = e; } });
    venc.configure({ codec, width: 400, height: hdr.height, bitrate: 1500000, framerate: hdr.fps, latencyMode: 'quality', avc: { format: 'avc' } });
    let aenc = null, rs = null, aOut = 0, pend = [], pendN = 0;
    if (hdr.audio) {
      aenc = new AudioEncoder({
        output: (c, m) => {
          if (m && m.decoderConfig && m.decoderConfig.description)
            m = { ...m, decoderConfig: { ...m.decoderConfig, description: asc(m.decoderConfig.description) } };
          muxer.addAudioChunk(c, m);
        },
        error: (e) => { error = e; },
      });
      aenc.configure({ codec: 'mp4a.40.2', sampleRate: AAC_RATE, numberOfChannels: 2, bitrate: 160000 });
      rs = new Resampler(hdr.audioRate / 1000, AAC_RATE);
    }
    let picture = -1, base = null;
    const usPerFrame = 1e6 / hdr.fps;
    const audioBlock = (s) => { // f32-planar: the format checked on the iPhone (tests/webcodecs)
      const frames = s.length / 2;
      const data = new Float32Array(frames * 2);
      for (let i = 0; i < frames; i++) { data[i] = s[2 * i] / 32768; data[frames + i] = s[2 * i + 1] / 32768; }
      const ad = new AudioData({ format: 'f32-planar', sampleRate: AAC_RATE, numberOfFrames: frames, numberOfChannels: 2, timestamp: Math.round(aOut * 1e6 / AAC_RATE), data });
      aOut += frames;
      aenc.encode(ad);
      ad.close();
    };
    return {
      frame(rgba, index, pic) {
        if (error) throw error;
        if (pic !== picture || !base) { // a new picture; repeats share its memory
          if (base) base.close();
          base = new VideoFrame(rgba, { format: 'RGBA', codedWidth: 400, codedHeight: hdr.height, timestamp: 0 });
          picture = pic;
        }
        const f = new VideoFrame(base, { timestamp: Math.round(index * usPerFrame), duration: Math.round(usPerFrame) });
        venc.encode(f, { keyFrame: index % (hdr.fps * 2) === 0 });
        f.close();
      },
      audio(s) {
        const r = rs.run(s);
        pend.push(r); pendN += r.length;
        if (pendN >= 2048 * 2) {
          const all = new Int16Array(pendN); let o = 0;
          for (const p of pend) { all.set(p, o); o += p.length; }
          pend = []; pendN = 0;
          audioBlock(all);
        }
      },
      async wait() { // keeps the encoders' queues short
        while (!error && (venc.encodeQueueSize > 24 || (aenc && aenc.encodeQueueSize > 24))) await sleep(4);
        if (error) throw error;
      },
      async finish() {
        if (aenc && pendN) {
          const all = new Int16Array(pendN); let o = 0;
          for (const p of pend) { all.set(p, o); o += p.length; }
          audioBlock(all);
        }
        if (base) base.close();
        await venc.flush();
        if (aenc) await aenc.flush();
        if (error) throw error;
        muxer.finalize();
        return new Blob([target.buffer], { type: 'video/mp4' });
      },
    };
  }

  /* ------------------------------------------------------------ live: MediaRecorder */

  function liveType() {
    if (typeof MediaRecorder === 'undefined' || !HTMLCanvasElement.prototype.captureStream) return null;
    const types = ['video/mp4;codecs=avc1.42E01E,mp4a.40.2', 'video/mp4;codecs=avc1,mp4a.40.2', 'video/mp4;codecs=avc1,opus',
      'video/mp4', 'video/webm;codecs=vp9,opus', 'video/webm;codecs=vp8,opus', 'video/webm'];
    const want = new URLSearchParams(location.search).get('type'); // tests: ?type=webm
    return types.filter((m) => !want || m.indexOf(want) >= 0)
      .find((m) => { try { return MediaRecorder.isTypeSupported(m); } catch (e) { return false; } }) || null;
  }

  // Plays the recording onto a canvas in real time and records the canvas and the sound. While the
  // download is behind (or the page is hidden) the recording pauses and so does the media clock.
  // The AudioContext itself keeps running (no suspend()), sound goes out in short pieces a little
  // ahead; a pause stops them and the rest is played again after it.
  function liveConvert(hdr, mime, onTime, ctx) {
    const canvas = $('canvas');
    canvas.height = hdr.height;
    const g = canvas.getContext('2d');
    const dest = ctx.createMediaStreamDestination();
    // A constant silent source: with nothing playing (silence is not stored, and nothing plays
    // while paused) Chrome's MP4 recorder stops taking sound for good (headless Chrome
    // 2026-11-05: 2-26 s of sound in a 36 s video). With it, pauses are fine (12 in a row).
    const keep = ctx.createConstantSource ? ctx.createConstantSource() : ctx.createOscillator();
    if (keep.offset) keep.offset.value = 0;
    const mute = ctx.createGain();
    mute.gain.value = keep.offset ? 1 : 0;
    keep.connect(mute);
    mute.connect(dest);
    keep.start();
    const stream = new MediaStream([...canvas.captureStream(hdr.fps).getVideoTracks(), ...dest.stream.getAudioTracks()]);
    const rec = new MediaRecorder(stream, { mimeType: mime, videoBitsPerSecond: 1500000, audioBitsPerSecond: 128000 });
    const parts = [];
    rec.ondataavailable = (e) => { if (e.data && e.data.size) parts.push(e.data); };
    const rate = hdr.audioRate / 1000;
    const PIECE = 4096;       // samples per scheduled piece (~0.125 s)
    const LOOKAHEAD = 0.4;    // seconds of sound scheduled ahead
    const pics = [];          // { index, img } distinct pictures, in order
    let lastIndex = -1, picture = -1;
    let audioIn = 0;          // samples received
    const queue = [];         // { pos, data } not scheduled yet, in order
    let playing = [];         // { src, at, pos, data } scheduled
    let t0 = 0, pausedTotal = 0, pausedAt = 0, started = false, paused = true, ended = false, done = false;
    const media = () => (!started ? 0 : (paused ? pausedAt : ctx.currentTime) - t0 - pausedTotal);
    const ctxTime = (m) => t0 + pausedTotal + m;
    const aheadVideo = () => (lastIndex + 1) / hdr.fps - media();
    // sound is known up to what arrived, and up to ~0.5 s before the last picture: in the file it
    // lags its video by < 0.3 s, and where nothing arrived the recording was silent (not stored)
    const aheadAudio = () => (hdr.audio ? Math.max(audioIn / rate, (lastIndex + 1) / hdr.fps - 0.5) - media() : 99);
    const scheduleAhead = () => {
      const until = media() + LOOKAHEAD;
      while (queue.length && queue[0].pos / rate < until) {
        const q = queue.shift();
        const n = q.data.length / 2;
        const buf = ctx.createBuffer(2, n, rate);
        const l = buf.getChannelData(0), r = buf.getChannelData(1);
        for (let i = 0; i < n; i++) { l[i] = q.data[2 * i] / 32768; r[i] = q.data[2 * i + 1] / 32768; }
        const src = ctx.createBufferSource();
        src.buffer = buf;
        src.connect(dest);
        const at = ctxTime(q.pos / rate), now = ctx.currentTime;
        if (at >= now) src.start(at);
        else src.start(now, Math.min(now - at, buf.duration)); // late: keep it in step with the picture
        playing.push({ src, at, pos: q.pos, data: q.data });
      }
      playing = playing.filter((p) => p.at + p.data.length / 2 / rate > ctx.currentTime - 1);
    };
    const pause = () => {
      if (paused || !started) return; // before the start nothing runs yet
      paused = true;
      pausedAt = ctx.currentTime;
      rec.pause();
      const back = [];
      for (const p of playing) { // what is not heard yet goes back to the queue
        const end = p.at + p.data.length / 2 / rate;
        if (end <= pausedAt) continue;
        try { p.src.stop(); } catch (e) { /* already stopped */ }
        const played = p.at < pausedAt ? Math.floor((pausedAt - p.at) * rate) : 0;
        back.push({ pos: p.pos + played, data: p.data.subarray(played * 2) });
      }
      playing = [];
      queue.unshift(...back);
    };
    const resume = () => {
      if (!paused || document.hidden) return;
      if (!started) {
        started = true;
        t0 = ctx.currentTime + 0.15;
        pausedTotal = 0;
        rec.start(1000);
      } else {
        pausedTotal += ctx.currentTime - pausedAt;
        rec.resume();
      }
      paused = false;
      scheduleAhead();
    };
    let finish = null;
    const tick = () => {
      if (done) return;
      const ok = ended || (aheadVideo() > 0.25 && aheadAudio() > 0.25);
      if (!paused && !ok) pause();
      if (paused && !document.hidden && (ended || (aheadVideo() > 0.8 && aheadAudio() > 0.6))) resume();
      if (!paused) {
        scheduleAhead();
        const idx = Math.floor(media() * hdr.fps);
        let k = -1;
        for (let i = 0; i < pics.length && pics[i].index <= idx; i++) k = i;
        if (k >= 0) { g.putImageData(pics[k].img, 0, 0); pics.splice(0, k); } // keep the shown one
        if (ended && idx > lastIndex + 2 && !queue.length) finish();
      }
      onTime(media(), paused);
      requestAnimationFrame(tick);
    };
    document.addEventListener('visibilitychange', () => { if (document.hidden) pause(); });
    const result = new Promise((resolve, reject) => {
      finish = () => {
        if (done) return;
        done = true;
        rec.onstop = () => { ctx.close(); resolve(new Blob(parts, { type: mime.split(';')[0] })); };
        rec.onerror = (e) => reject(e.error || new Error('MediaRecorder'));
        setTimeout(() => rec.stop(), 200); // the last sound piece reaches the recorder
      };
    });
    requestAnimationFrame(tick);
    let pend = [], pendN = 0;
    const pushAudio = (force) => {
      while (pendN >= PIECE * 2 || (force && pendN)) {
        const n = Math.min(pendN, PIECE * 2);
        const piece = new Int16Array(n);
        let o = 0;
        while (o < n) {
          const h = pend[0], k = Math.min(h.length, n - o);
          piece.set(h.subarray(0, k), o);
          o += k;
          if (k === h.length) pend.shift(); else pend[0] = h.subarray(k);
        }
        pendN -= n;
        queue.push({ pos: audioIn - (pendN + n) / 2, data: piece });
      }
    };
    return {
      frame(rgba, index, pic) {
        if (pic !== picture) {
          pics.push({ index, img: new ImageData(new Uint8ClampedArray(rgba), 400, hdr.height) });
          picture = pic;
        }
        lastIndex = index;
      },
      audio(s) {
        pend.push(s);
        pendN += s.length;
        audioIn += s.length / 2;
        pushAudio(false);
      },
      hungry() { // decoded pictures are large: keep about a second ahead of what is shown
        return !done && (aheadVideo() < 1.2 || (aheadAudio() < 1.2 && aheadVideo() < 8));
      },
      async finish() {
        pushAudio(true);
        ended = true;
        return result;
      },
    };
  }

  /* ------------------------------------------------------------ flow */

  async function run(mode) {
    const url = '/rec/' + encodeURIComponent(name);
    let enc = null, hdr = null, firstErr = null;
    const conv = new R3D.Converter({
      onHeader: (h) => { hdr = h; },
      onFrame: (rgba, i, pic) => {
        if (!enc) return;
        enc.frame(rgba, i, pic);
        if (mode.fast && i % 15 === 0) { // live mode reports its own clock
          const sec = i / hdr.fps;
          step(2, 'on', totalSec ? (sec / totalSec) * 100 : 0, clock(sec));
        }
      },
      onAudio: (s) => { if (enc) enc.audio(s); },
    });
    const totalSec = info.ms / 1000;
    step(1, 'on', 0, '0%');
    step(2, 'on', 0, '', mode.label);
    const onTime = (sec, paused) => {
      step(2, 'on', totalSec ? (sec / totalSec) * 100 : 0, clock(sec), paused ? (document.hidden ? t('hidden') : t('paused')) : mode.label);
    };
    let dlDone = false;
    const ensureEnc = () => { if (!enc && hdr) enc = mode.make(hdr, onTime); };
    const stash = []; // live: downloaded, not decoded yet
    const feed = mode.fast
      ? async (piece) => {
        try {
          conv.push(piece);
          ensureEnc();
          if (enc) await enc.wait();
        } catch (e) { firstErr = firstErr || e; }
      }
      : async (piece) => { // a fast link fills it faster than real time: keep it bounded
        while (!firstErr && stash.reduce((n, p) => n + p.length, 0) > (16 << 20)) await sleep(100);
        stash.push(piece);
      };
    const pump = mode.fast ? Promise.resolve() : (async () => {
      for (;;) {
        if (firstErr) return;
        if (stash.length && (!enc || enc.hungry())) {
          try {
            conv.push(stash.shift());
            ensureEnc();
          } catch (e) { firstErr = firstErr || e; }
          continue;
        }
        if (dlDone && !stash.length) return;
        await sleep(20);
      }
    })();
    const dl = new Download(url, info.size, feed, (g, size, rate) => {
      const left = rate ? (size - g) / rate : 0;
      step(1, g >= size ? 'done' : 'on', (g / size) * 100, Math.floor((g / size) * 100) + '%',
        `${mb(g)} / ${mb(size)} · ${(rate / 1048576).toFixed(2)} ${t('mbs')}` + (g < size ? ' · ' + t('left', duration(left)) : ''));
    });
    const go = async () => {
      try {
        await dl.run();
      } catch (e) {
        report('r3d download failed: ' + why(e));
        fail(e.notFound ? t('notFound') : t('lost'), e.notFound ? null : go);
        return;
      }
      report(`r3d downloaded: ${dl.got} B in ${((performance.now() - dl.t0) / 1000).toFixed(1)} s`);
      dlDone = true;
      step(1, 'done', 100, t('ready'));
      try {
        await pump;
        if (firstErr) throw firstErr;
        conv.finish();
        if (!enc) throw new Error('no video');
        const blob = await enc.finish();
        step(2, 'done', 100, t('ready'), mode.label);
        offer(blob);
      } catch (e) {
        report('browser video failed: ' + why(e));
        fail(t('failed', e && e.message ? e.message : e));
      }
    };
    go();
  }

  function offer(blob) {
    step(3, 'on');
    const ext = blob.type.indexOf('webm') >= 0 ? 'webm' : 'mp4';
    const href = URL.createObjectURL(blob);
    const m = /^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})/.exec(name) || [];
    const fname = `3DS ${m[1]}-${m[2]}-${m[3]} ${m[4]}-${m[5]}-${m[6]}.${ext}`;
    $('preview').src = href;
    const save = $('save');
    save.href = href;
    save.download = fname;
    save.textContent = t('save') + ' · ' + mb(blob.size);
    $('howto').textContent = isIOS ? t('howIos') : isAndroid ? t('howAndroid') : t('howOther');
    $('keep').hidden = true;
    $('result').hidden = false;
    report(`offer ${fname}: ${blob.type} ${blob.size} B, share ${!!navigator.canShare}`);
    // Web Share with files exists only on https pages; offer it where it does
    const file = typeof File !== 'undefined' ? new File([blob], fname, { type: blob.type }) : null;
    if (file && navigator.canShare && navigator.canShare({ files: [file] })) {
      const b = document.createElement('button');
      b.className = 'secondary';
      b.textContent = t('share');
      b.onclick = () => navigator.share({ files: [file] }).then(() => report('shared'), (e) => report('share: ' + why(e)));
      save.after(b);
    }
    save.addEventListener('click', () => { report('save tapped'); step(3, 'done'); });
  }

  async function main() {
    document.querySelectorAll('[data-t]').forEach((el) => { el.textContent = t(el.dataset.t); });
    report(`open ${name} | ${ua} | screen ${screen.width}x${screen.height}@${devicePixelRatio} | ${navigator.language}` +
      ` | mem ${navigator.deviceMemory || '?'} GB | secure ${window.isSecureContext} | VideoEncoder ${typeof VideoEncoder}` +
      ` | MediaRecorder ${typeof MediaRecorder}`);
    try {
      const r = await fetch('/api/rec', { cache: 'no-store' });
      info = await r.json();
    } catch (e) { fail(t('lost'), main); return; }
    if (!name) name = info.name;
    if (!name) { fail(t('noFile')); return; }
    report(`rec ${info.name} ${info.size} B ${info.ms} ms ok ${info.ok} closed ${info.closed} | prep ${info.prep} ${info.prepPct} mp4 ${info.mp4}`);
    if (info.lang === 'uk') { L = TEXT.uk; document.documentElement.lang = 'uk'; }
    document.querySelectorAll('[data-t]').forEach((el) => { el.textContent = t(el.dataset.t); });
    if (info.name !== name || !info.ok) { fail(t('notFound')); return; }
    $('console').textContent = info.console || '';
    $('when').textContent = when(name);
    $('fname').textContent = name;
    $('len').textContent = clock(info.ms / 1000);
    const raw = $('raw');
    raw.href = '/rec/' + encodeURIComponent(name);
    raw.download = name;
    raw.textContent = t('raw');
    $('sd').textContent = t('sd');
    $('diag').addEventListener('click', () => report('diagnostics file saved'));
    step(3, '');
    if (info.prep && info.prep !== 'failed') consoleVideo();
    else browserVideo();
  }

  /* ------------------------------------------------------------ the console's MP4 */

  async function consoleVideo() {
    $('s1').querySelector('b').textContent = t('c1');
    $('s2').querySelector('b').textContent = t('c2');
    $('error').hidden = true;
    let st = info, fails = 0, seen = null, left = 0;
    const since = performance.now();
    step(2, '', 0, t('waiting'), '');
    while (st.prep !== 'ready') {
      if (st.prep === 'failed' || (st.prep === 'none' && performance.now() - since > 20000)) {
        $('s1').querySelector('b').textContent = t('s1');
        $('s2').querySelector('b').textContent = t('s2');
        report(`console MP4 ${st.prep}: the browser makes the video`);
        browserVideo(); // the console could not make it: the browser does
        return;
      }
      const pct = (st.prepPct || 0) / 10, now = performance.now();
      if (!seen && pct > 0) seen = { pct, at: now };
      else if (seen && pct > seen.pct + 1) left = ((100 - pct) * (now - seen.at)) / (pct - seen.pct) / 1000;
      step(1, 'on', pct, Math.floor(pct) + '%',
        (st.prep === 'working' ? t('c1x') : t('c1q')) + (left ? ' · ' + t('left', duration(left)) : ''));
      await sleep(1000);
      try {
        st = await (await fetch('/api/rec', { cache: 'no-store' })).json();
        fails = 0;
      } catch (e) {
        if (++fails >= 5) { fail(t('lost'), consoleVideo); return; }
      }
    }
    step(1, 'done', 100, t('ready'), '');
    report(`console MP4 ready: ${st.mp4} B after ${Math.round((performance.now() - since) / 1000)} s on the page`);
    $('size').textContent = mb(st.mp4);
    $('eta').textContent = duration(st.mp4 / (WIFI_KBS * 1024));
    const stem = name.replace(/\.r3d$/, '');
    const m = /^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})/.exec(name) || [];
    const direct = $('direct');
    direct.href = '/rec/' + encodeURIComponent(stem + '.mp4');
    direct.download = `3DS ${m[1]}-${m[2]}-${m[3]} ${m[4]}-${m[5]}-${m[6]}.mp4`;
    if (st.mp4 > BIG_MP4) { // too big to hold in a phone's memory: the browser's own download
      direct.textContent = t('direct') + ' · ' + mb(st.mp4);
      direct.hidden = false;
      show('long', t('big', mb(st.mp4)));
      step(2, 'on', 0, '', '');
      direct.addEventListener('click', () => { report('big MP4: direct download tapped'); step(2, 'done', 100, t('ready')); });
      report('big MP4: direct link offered');
      return;
    }
    const parts = [];
    const dl = new Download('/rec/' + encodeURIComponent(stem + '.mp4'), st.mp4, async (piece) => { parts.push(piece); },
      (g, size, rate) => {
        const rest = rate ? (size - g) / rate : 0;
        step(2, g >= size ? 'done' : 'on', (g / size) * 100, Math.floor((g / size) * 100) + '%',
          `${mb(g)} / ${mb(size)} · ${(rate / 1048576).toFixed(2)} ${t('mbs')}` + (g < size ? ' · ' + t('left', duration(rest)) : ''));
      });
    try {
      await dl.run();
    } catch (e) {
      report('MP4 download failed: ' + why(e));
      fail(e.notFound ? t('notFound') : t('lost'), e.notFound ? null : consoleVideo);
      return;
    }
    const secs = (performance.now() - dl.t0) / 1000;
    report(`MP4 downloaded: ${dl.got} B in ${secs.toFixed(1)} s (${Math.round(dl.got / 1024 / secs)} KB/s)`);
    step(2, 'done', 100, t('ready'));
    offer(new Blob(parts, { type: 'video/mp4' }));
    direct.addEventListener('click', () => report('direct link tapped after the page download'));
    direct.className = 'btn link'; // if saving from the page does not work on some phone
    direct.textContent = t('directLink');
    direct.hidden = false;
  }

  /* ------------------------------------------------------------ the browser makes it */

  async function browserVideo() {
    $('size').textContent = mb(info.size);
    const etaSec = info.size / (WIFI_KBS * 1024);
    $('eta').textContent = duration(etaSec);
    if (etaSec > 10 * 60) show('long', t('long', duration(etaSec)));
    step(1, 'on', 0, t('waiting'));
    step(2, '', 0, t('waiting'));

    const height = info.both ? 480 : 240;
    const codec = await fastSupport(height, info.fps || 30, info.audio);
    if (codec) {
      report('browser video: fast, ' + JSON.stringify(codec).slice(0, 120));
      run({ fast: true, label: t('fast'), make: (hdr) => fastConvert(hdr, codec) });
      return;
    }
    const mime = liveType();
    report('browser video: ' + (mime ? 'live, ' + mime : 'none possible'));
    if (!mime) {
      step(2, '', 0, '', t('none'));
      return; // the original .r3d link stays
    }
    show('keep', t('keep'));
    const b = $('start');
    b.textContent = t('start');
    b.hidden = false;
    $('s2x').textContent = t('startHint');
    b.onclick = () => {
      b.hidden = true;
      // created and woken inside the tap: browsers let audio start only from a user gesture
      const ctx = new (window.AudioContext || window.webkitAudioContext)();
      ctx.resume();
      run({ label: t('live'), make: (hdr, onTime) => liveConvert(hdr, mime, onTime, ctx) });
    };
  }

  main();
})();
