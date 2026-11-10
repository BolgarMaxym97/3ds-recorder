/*
 * .r3d decoding for the phone page: a JS port of tools/r3dconv (codec/include/r3d_format.h,
 * codec/source/codec.c XR32 and Q565, codec/source/huff.c, tools/r3dconv/audio_place.h + audio_eq.h). It must give the same
 * pictures and the same samples as r3dconv (tests/web/compare.mjs checks it byte for byte).
 *
 *   const conv = new R3D.Converter({ onHeader, onFrame, onAudio, onEnd });
 *   conv.push(uint8Array) ... (the file's bytes in order, any piece sizes)
 *   conv.finish()
 *
 * onHeader(h)           { fps, both, audio, audioRate (mHz), width 400, height 480 / 240 }
 * onFrame(rgba, index, picture)  one output frame at h.fps (constant rate, as r3dconv emits
 *                       them): RGBA, width x height, top screen above, bottom centred under it
 *                       (a bottom-screen-only recording: 400 x 240, the bottom screen centred).
 *                       The buffer is reused: copy or encode it before returning. `picture`
 *                       changes only when the image does (static scenes repeat one picture).
 * onAudio(int16)        stereo samples (L R L R ...) at audioRate / 1000 Hz, in order, gaps as
 *                       silence, lost frames filled in, through the treble cut.
 * onEnd(info)           { frames, seconds, audioSamples }
 */
(function (root) {
  'use strict';

  const M = {
    FILE: 0x42443352, FRAME: 0x46443352, XFRAME: 0x58443352, YFRAME: 0x59443352, HFRAME: 0x48443352, QFRAME: 0x51443352,
    PAD: 0x50443352, AUDIO: 0x41443352, DSPMEM: 0x4d443352, END: 0x45443352,
  };
  const SCREEN_KEY = 0x40;
  const MODE_FULL_TOP = 3, MODE_FULL_BOTTOM = 5;
  const AUDIO_RATE_MHZ = 32728498;
  const AUDIO_RAW = 2;

  // audio_place.h
  const AUDIO_BREAK_US = 30000;
  const AUDIO_BEHIND_FRAMES = 2;
  const AUDIO_BEHIND_HARSH_PCT = 70;
  const AUDIO_AHEAD_FRAMES = 8;
  const AUDIO_CONCEAL_MAX = 8;
  // audio_eq.h
  const EQ_FREQ_HZ = 4000.0, EQ_GAIN_DB = -8.0, EQ_Q = 0.70710678;

  /* ------------------------------------------------------------ XR32 (codec.c) */

  // Returns false on a damaged frame. `frame` holds the previous picture (delta frames).
  function xr32Decode(inp, frame, key) {
    const n = frame.length;
    const len = inp.length;
    let pos = 0, i = 0;
    while (pos + 2 <= len) {
      const t = inp[pos] | (inp[pos + 1] << 8);
      pos += 2;
      const words = ((t & 0x7fff) + 1) * 2;
      if (i + words > n) return false;
      if (t & 0x8000) {
        if (pos + words * 2 > len) return false;
        if (key) {
          for (let k = 0; k < words; k++, pos += 2) frame[i + k] = inp[pos] | (inp[pos + 1] << 8);
        } else {
          for (let k = 0; k < words; k++, pos += 2) frame[i + k] ^= inp[pos] | (inp[pos + 1] << 8);
        }
      } else if (key) {
        frame.fill(0, i, i + words);
      }
      i += words;
    }
    return pos === len && i === n;
  }

  /* ------------------------------------------------------------ Q565 (codec.c) */

  // A frame on its own (codec_q565_decode). Returns false on a damaged frame.
  function q565Decode(inp, frame) {
    const n = frame.length, size = inp.length, index = new Uint16Array(64);
    let prev = 0, pos = 0, i = 0;
    const hash = (px) => Math.imul(px, 0x9e3779b1) >>> 26;
    while (i < n) {
      if (pos >= size) return false;
      const b = inp[pos++];
      if (b === 0xfe) {
        if (pos + 2 > size) return false;
        prev = inp[pos] | (inp[pos + 1] << 8);
        pos += 2;
        index[hash(prev)] = prev;
      } else if ((b & 0xc0) === 0xc0) {
        const k = (b & 0x3f) + 1;
        if (i + k > n) return false;
        frame.fill(prev, i, i + k);
        i += k;
        continue;
      } else if ((b & 0xc0) === 0) {
        prev = index[b];
      } else {
        let sr, sg, sb;
        if ((b & 0xc0) === 0x40) {
          sr = ((b >> 4) & 3) - 2;
          sg = ((b >> 2) & 3) - 2;
          sb = (b & 3) - 2;
        } else {
          if (pos >= size) return false;
          const b2 = inp[pos++];
          sg = (b & 0x3f) - 32;
          const half = ((sg + 32) >> 1) - 16;
          sr = (b2 >> 4) - 8 + half;
          sb = (b2 & 15) - 8 + half;
        }
        const r = ((prev >> 11) + sr) & 31, g = (((prev >> 5) & 63) + sg) & 63, bl = ((prev & 31) + sb) & 31;
        prev = (r << 11) | (g << 5) | bl;
        index[hash(prev)] = prev;
      }
      frame[i++] = prev;
    }
    return pos === size;
  }

  /* ------------------------------------------------------------ Huffman over XR32 (huff.c) */

  const HUFF_MAX_BITS = 12, HUFF_HEADER = 4 + 256;
  const huffTables = [new Uint16Array(1 << HUFF_MAX_BITS), new Uint16Array(1 << HUFF_MAX_BITS)];

  // Decoding table from 128 packed bytes of code lengths; false if they are not a valid code.
  function huffTable(inp, at, table) {
    const len = new Uint8Array(256);
    let kraft = 0;
    for (let k = 0; k < 128; k++) {
      len[2 * k] = inp[at + k] & 15;
      len[2 * k + 1] = inp[at + k] >> 4;
    }
    for (let sym = 0; sym < 256; sym++) {
      if (len[sym] > HUFF_MAX_BITS) return false;
      if (len[sym]) kraft += 1 << (HUFF_MAX_BITS - len[sym]);
    }
    if (kraft > (1 << HUFF_MAX_BITS)) return false;
    table.fill(0);
    // canonical codes (build_codes), bit-reversed: least significant bit first
    const count = new Uint32Array(HUFF_MAX_BITS + 1), next = new Uint32Array(HUFF_MAX_BITS + 2);
    for (let sym = 0; sym < 256; sym++) count[len[sym]]++;
    count[0] = 0;
    let c = 0;
    for (let b = 1; b <= HUFF_MAX_BITS; b++) {
      c = (c + count[b - 1]) << 1;
      next[b] = c;
    }
    for (let sym = 0; sym < 256; sym++) {
      const l = len[sym];
      if (!l) continue;
      let v = next[l]++, r = 0;
      for (let k = 0; k < l; k++, v >>>= 1) r = (r << 1) | (v & 1);
      const e = (sym << 4) | l;
      for (let x = r; x < (1 << HUFF_MAX_BITS); x += 1 << l) table[x] = e;
    }
    return true;
  }

  // An R3DH frame straight into the picture (huff_xr32_decode). Returns false on a damaged frame.
  function huffXr32Decode(inp, frame, key) {
    const size = inp.length, n = frame.length;
    if (size < HUFF_HEADER) return false;
    const raw = (inp[0] | (inp[1] << 8) | (inp[2] << 16) | (inp[3] << 24)) >>> 0;
    if (raw & 1) return false;
    const t0 = huffTables[0], t1 = huffTables[1];
    if (!huffTable(inp, 4, t0) || !huffTable(inp, 4 + 128, t1)) return false;
    let p = HUFF_HEADER, acc = 0, nb = 0, zf = 0, bad = false;
    const sym = (tab) => {
      if (nb < 16) {
        if (p + 2 <= size) {
          acc = (acc | ((inp[p] | (inp[p + 1] << 8)) << nb)) >>> 0;
          p += 2;
        } else {
          zf += 16;
        }
        nb += 16;
      }
      const e = tab[acc & 0xfff], l = e & 15;
      if (!l) { bad = true; return 0; }
      acc >>>= l;
      nb -= l;
      return e >> 4;
    };
    let left = raw / 2, i = 0;
    while (left) {
      const t = sym(t0) | (sym(t1) << 8);
      if (bad) return false;
      left--;
      const words = ((t & 0x7fff) + 1) * 2;
      if (i + words > n) return false;
      if (t & 0x8000) {
        if (words > left) return false;
        left -= words;
        for (let k = 0; k < words; k++) {
          const v = sym(t0) | (sym(t1) << 8);
          if (key) frame[i + k] = v; else frame[i + k] ^= v;
        }
        if (bad || zf > 32) return false;
      } else if (key) {
        frame.fill(0, i, i + words);
      }
      i += words;
    }
    return i === n && zf <= nb;
  }

  /* ------------------------------------------------------------ audio (audio_place.h, audio_eq.h) */

  // C's lrint (round half to even), for the filter's output
  function rint(y) {
    const r = Math.round(y);
    return (r - y === 0.5 && (r & 1)) ? r - 1 : r;
  }

  class Eq {
    constructor(rateHz) {
      const A = Math.pow(10.0, EQ_GAIN_DB / 40.0), w0 = 2.0 * Math.PI * EQ_FREQ_HZ / rateHz;
      const c = Math.cos(w0), alpha = Math.sin(w0) / (2.0 * EQ_Q), sa = 2.0 * Math.sqrt(A) * alpha;
      const a0 = (A + 1) - (A - 1) * c + sa;
      this.b0 = A * ((A + 1) + (A - 1) * c + sa) / a0;
      this.b1 = -2.0 * A * ((A - 1) + (A + 1) * c) / a0;
      this.b2 = A * ((A + 1) + (A - 1) * c - sa) / a0;
      this.a1 = 2.0 * ((A - 1) - (A + 1) * c) / a0;
      this.a2 = ((A + 1) - (A - 1) * c - sa) / a0;
      this.z1 = [0, 0];
      this.z2 = [0, 0];
    }
    run(s) { // Int16Array stereo, in place
      const { b0, b1, b2, a1, a2, z1, z2 } = this;
      for (let i = 0; i < s.length; i += 2) {
        for (let ch = 0; ch < 2; ch++) {
          const x = s[i + ch];
          let y = b0 * x + z1[ch];
          z1[ch] = b1 * x - a1 * y + z2[ch];
          z2[ch] = b2 * x - a2 * y;
          y = y > 32767.0 ? 32767.0 : y < -32768.0 ? -32768.0 : y;
          s[i + ch] = rint(y);
        }
      }
    }
  }

  class Placer {
    constructor(rateMilliHz) {
      this.rate = BigInt(rateMilliHz);
      this.out = 0n; // samples placed so far
      this.lastUs = 0;
      this.started = false;
      this.catchingUp = false;
      this.conceal = 0;
    }
    due(timeUs) { return BigInt(timeUs) * this.rate / 1000000000n; }
    // samples of silence before this frame, or -1 to drop it; this.conceal: frames to fill in
    place(timeUs, frameSamples, harshJoin) {
      const due = this.due(timeUs);
      const n = BigInt(frameSamples);
      const ahead = n * BigInt(AUDIO_AHEAD_FRAMES);
      const runStart = !this.started || timeUs - this.lastUs > AUDIO_BREAK_US;
      this.started = true;
      this.lastUs = timeUs;
      this.conceal = 0;
      let silence = 0n;
      if (runStart || this.catchingUp) {
        if (due > this.out) silence = due - this.out;
        else if (this.out > due + ahead) {
          this.catchingUp = true;
          return -1;
        }
        this.catchingUp = false;
      } else if (this.out > due + ahead) {
        return -1;
      } else {
        while (this.conceal < AUDIO_CONCEAL_MAX &&
               (due > this.out + n * BigInt(AUDIO_BEHIND_FRAMES + this.conceal) ||
                (harshJoin && due * 100n > (this.out + n * BigInt(this.conceal)) * 100n + n * BigInt(AUDIO_BEHIND_HARSH_PCT)))) {
          this.conceal++;
        }
      }
      this.out += silence + n * BigInt(this.conceal) + n;
      return Number(silence);
    }
    padTo(timeUs) {
      const due = this.due(timeUs);
      return due > this.out ? Number(due - this.out) : 0;
    }
  }

  function iabs(v) { return v < 0 ? -v : v; }

  function joinHarsh(prev, next, n) {
    let inner = 0;
    for (let k = 1; k < n; k++) inner += iabs(next[2 * k] - next[2 * k - 2]) + iabs(next[2 * k + 1] - next[2 * k - 1]);
    const avg = inner / (2.0 * (n - 1)) + 1.0;
    let err = 0;
    for (let ch = 0; ch < 2; ch++) {
      const p1 = prev[2 * (n - 1) + ch], p2 = prev[2 * (n - 2) + ch];
      const c0 = next[ch], c1 = next[2 + ch];
      err += iabs(c0 - (2 * p1 - p2)) + iabs(p1 - (2 * c0 - c1));
    }
    return err / 4.0 / avg > 4.0;
  }

  function pingpong(i, n) {
    const q = i % (2 * n);
    return q < n ? n - 1 - q : q - n;
  }

  function conceal(prev, next, n, frames) {
    const len = n * frames;
    const out = new Int16Array(len * 2);
    for (let j = 0; j < len; j++) {
      const w = 0.5 - 0.5 * Math.cos(3.14159265358979323846 * (j + 0.5) / len);
      const a = pingpong(j, n), b = n - 1 - pingpong(len - 1 - j, n);
      for (let ch = 0; ch < 2; ch++) {
        const v = (1.0 - w) * prev[2 * a + ch] + w * next[2 * b + ch];
        out[2 * j + ch] = Math.trunc(v < 0 ? v - 0.5 : v + 0.5);
      }
    }
    return out;
  }

  // audio_place.h: soft edges around silence (audio_tail, audio_fade_in)
  const AUDIO_EDGE_FADE = 80;

  function tail(prev, n, silence) {
    const len = silence < n ? silence : n;
    const out = new Int16Array(len * 2);
    for (let j = 0; j < len; j++) {
      const w = 0.5 + 0.5 * Math.cos(3.14159265358979323846 * (j + 0.5) / len);
      const a = pingpong(j, n);
      for (let ch = 0; ch < 2; ch++) {
        const v = w * prev[2 * a + ch];
        out[2 * j + ch] = Math.trunc(v < 0 ? v - 0.5 : v + 0.5);
      }
    }
    return out;
  }

  function fadeIn(frame, n) {
    const len = n < AUDIO_EDGE_FADE ? n : AUDIO_EDGE_FADE;
    for (let j = 0; j < len; j++) {
      const w = 0.5 - 0.5 * Math.cos(3.14159265358979323846 * (j + 0.5) / len);
      for (let ch = 0; ch < 2; ch++) {
        const v = w * frame[2 * j + ch];
        frame[2 * j + ch] = Math.trunc(v < 0 ? v - 0.5 : v + 0.5);
      }
    }
  }

  /* ------------------------------------------------------------ converter */

  class Converter {
    constructor(cb) {
      this.cb = cb;
      this.buf = new Uint8Array(1 << 20);
      this.len = 0;   // bytes in buf
      this.pos = 0;   // parsed up to
      this.hdr = null;
      this.done = false;
      this.emitted = 0;
      this.lastUs = 0;
      this.lastWritten = null;
    }

    push(chunk) {
      if (this.done) return;
      if (this.len + chunk.length > this.buf.length && this.pos > 0) { // drop what is parsed
        this.buf.copyWithin(0, this.pos, this.len);
        this.len -= this.pos;
        this.pos = 0;
      }
      if (this.len + chunk.length > this.buf.length) {
        const nb = new Uint8Array(Math.max(this.buf.length * 2, this.len + chunk.length));
        nb.set(this.buf.subarray(0, this.len));
        this.buf = nb;
      }
      this.buf.set(chunk, this.len);
      this.len += chunk.length;
      this.parse();
    }

    parse() {
      const dv = () => new DataView(this.buf.buffer, this.buf.byteOffset);
      if (!this.hdr) {
        if (this.len < 32) return;
        const d = dv();
        if (d.getUint32(0, true) !== M.FILE) {
          this.done = true;
          throw new Error('not a .r3d file');
        }
        this.start(d);
        this.pos = 32;
      }
      while (!this.done && this.len - this.pos >= 16) {
        const d = dv(), p = this.pos;
        const h = {
          magic: d.getUint32(p, true), len: d.getUint32(p + 4, true), timeUs: d.getUint32(p + 8, true),
          screen: d.getUint8(p + 12), srcFormat: d.getUint8(p + 13), cols: d.getUint16(p + 14, true),
        };
        if (h.magic === M.END) { this.done = true; break; }
        const delta = h.magic === M.XFRAME || h.magic === M.YFRAME || h.magic === M.HFRAME || h.magic === M.QFRAME;
        const known = delta || h.magic === M.FRAME || h.magic === M.PAD || h.magic === M.AUDIO || h.magic === M.DSPMEM;
        if (!known) { this.done = true; break; } // the end of a cut-off file
        const stored = delta ? (h.len + 15) & ~15 : h.len;
        if (h.magic === M.AUDIO && (h.len < h.cols * 4 || h.len > 4096)) { this.done = true; break; }
        if (this.len - p - 16 < stored) return; // wait for the rest
        if (h.magic !== M.PAD && h.magic !== M.DSPMEM) this.record(h, this.buf.subarray(p + 16, p + 16 + h.len));
        this.pos = p + 16 + stored;
      }
    }

    start(d) {
      const version = d.getUint32(4, true), mode = d.getUint8(17), fps0 = d.getUint8(18);
      const audioRate = version >= 4 ? d.getUint32(20, true) : 0;
      let fps = version >= 3 && fps0 ? fps0 : 30;
      if (fps === 0 || fps > 60) fps = 30;
      const bottomOnly = mode === MODE_FULL_BOTTOM; // 400 x 240, the bottom screen centred
      const both = mode !== MODE_FULL_TOP && !bottomOnly;
      this.hdr = { fps, both, audio: audioRate !== 0, audioRate: audioRate || AUDIO_RATE_MHZ, width: 400, height: both ? 480 : 240 };
      this.scr = [
        { cols: 400, px: new Uint16Array(400 * 240), needKey: true, seen: false, ox: 0, oy: 0, shown: !bottomOnly },
        { cols: 320, px: new Uint16Array(320 * 240), needKey: true, seen: false, ox: 40, oy: both ? 240 : 0, shown: both || bottomOnly },
      ];
      this.rgba = new Uint8ClampedArray(400 * this.hdr.height * 4);
      for (let i = 3; i < this.rgba.length; i += 4) this.rgba[i] = 255;
      this.placer = new Placer(this.hdr.audioRate);
      this.eq = new Eq(this.hdr.audioRate / 1000.0);
      this.audioSamples = 0;
      this.picture = 0;
      this.cb.onHeader && this.cb.onHeader(this.hdr);
    }

    putScreen(s) {
      if (!s.seen || !s.shown) return;
      const out = this.rgba, rows = 240;
      for (let y = 0; y < rows; y++) {
        let o = ((s.oy + y) * 400 + s.ox) * 4;
        for (let x = 0; x < s.cols; x++, o += 4) {
          const v = s.px[x * rows + (rows - 1 - y)]; // column-major, word 0 = bottom row
          out[o] = ((v >> 11) & 0x1f) * 255 / 31 | 0;
          out[o + 1] = ((v >> 5) & 0x3f) * 255 / 63 | 0;
          out[o + 2] = (v & 0x1f) * 255 / 31 | 0;
        }
      }
      this.picture++;
    }

    record(h, payload) {
      if (h.magic === M.AUDIO) return this.audio(h, payload);
      const sc = h.screen & 0x3f;
      if (sc > 1 || h.len > 400 * 240 * 2 + 4096 + 16) { this.done = true; return; }
      const s = this.scr[sc];
      if (h.cols !== s.cols) return;
      // frames up to this record's time, with the pictures so far
      while (this.emitted * 1000000 / this.hdr.fps < h.timeUs) {
        this.cb.onFrame && this.cb.onFrame(this.rgba, this.emitted, this.picture);
        this.emitted++;
      }
      this.lastUs = h.timeUs;
      const key = (h.screen & SCREEN_KEY) !== 0;
      let ok;
      if (h.magic === M.FRAME) {
        ok = h.len >= s.px.length * 2;
        if (ok) for (let i = 0; i < s.px.length; i++) s.px[i] = payload[2 * i] | (payload[2 * i + 1] << 8);
      } else if (!key && s.needKey) {
        return; // waiting for a keyframe after a damaged frame
      } else if (h.magic === M.YFRAME) {
        ok = xr32Decode(payload, s.px, key);
      } else if (h.magic === M.HFRAME) {
        ok = huffXr32Decode(payload, s.px, key);
      } else if (h.magic === M.QFRAME) {
        ok = q565Decode(payload, s.px);
      } else {
        ok = false; // XOR-RLE: bench recordings only
      }
      if (!ok) { s.needKey = true; return; }
      s.needKey = false;
      s.seen = true;
      this.putScreen(s);
    }

    audio(h, payload) {
      const bytes = h.cols * 4;
      if (h.len >= bytes + 16) {
        const flags = payload[bytes + 6] | (payload[bytes + 7] << 8);
        if (flags & AUDIO_RAW) return;
      }
      if (h.cols !== 160 || !this.hdr.audio) return;
      const pcm = new Int16Array(320);
      for (let i = 0; i < 320; i++) pcm[i] = (payload[2 * i] | (payload[2 * i + 1] << 8)) << 16 >> 16;
      const harsh = this.lastWritten !== null && joinHarsh(this.lastWritten, pcm, 160);
      const silence = this.placer.place(h.timeUs, 160, harsh);
      if (silence < 0) return;
      let t = 0;
      if (silence > 0 && this.lastWritten !== null) {
        const fade = tail(this.lastWritten, 160, silence);
        t = fade.length / 2;
        this.out(fade);
      }
      this.silence(silence - t);
      if (this.placer.conceal) {
        const fill = conceal(this.lastWritten !== null ? this.lastWritten : pcm, pcm, 160, this.placer.conceal);
        this.out(fill);
      }
      const frame = pcm.slice(); // pcm stays unfiltered for the next join and fill
      if (silence > 0) fadeIn(frame, 160);
      this.out(frame);
      this.lastWritten = pcm;
    }

    out(s) {
      this.eq.run(s);
      this.audioSamples += s.length / 2;
      this.cb.onAudio && this.cb.onAudio(s);
    }

    silence(n) {
      while (n > 0) { // zeros too: the filter's tail continues into the silence
        const k = n > 1024 ? 1024 : n;
        this.out(new Int16Array(k * 2));
        n -= k;
      }
    }

    finish() {
      this.done = true;
      if (!this.hdr) return;
      if (this.hdr.audio) this.silence(this.placer.padTo(this.lastUs));
      this.cb.onEnd && this.cb.onEnd({ frames: this.emitted, seconds: this.lastUs / 1e6, audioSamples: this.audioSamples });
    }
  }

  root.R3D = { Converter, xr32Decode };
  if (typeof module !== 'undefined') module.exports = root.R3D;
})(typeof globalThis !== 'undefined' ? globalThis : this);
