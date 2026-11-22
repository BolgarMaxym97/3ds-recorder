// Button presses kept with a recording (R3DI records) and the overlay that shows them: the same
// shapes and colours as codec/source/inputs.c (the console's MP4 and r3dconv), drawn at any scale.
// tests/web/compare.mjs checks the two agree.
(function (root) {
  'use strict';

  const TOUCH_NONE = 0xff;
  const KEY = { A: 1, B: 2, SELECT: 4, START: 8, RIGHT: 16, LEFT: 32, UP: 64, DOWN: 128, R: 256, L: 512, X: 1024, Y: 2048 };
  const PANEL_W = 116, PANEL_H = 60, TOUCH_BOX = 24;

  // The samples of a recording, in time order: { us, keys, cx, cy, tx, ty }.
  class InputTrack {
    constructor() { this.list = []; this.i = 0; this.now = { us: 0, keys: 0, cx: 0, cy: 0, tx: TOUCH_NONE, ty: TOUCH_NONE }; }
    add(timeUs, count, payload) {
      if (!count || count > 64 || payload.length < count * 8) return 0;
      const d = new DataView(payload.buffer, payload.byteOffset, payload.length);
      for (let k = 0; k < count; k++) {
        const o = k * 8, tx = d.getUint8(o + 6), ty = d.getUint8(o + 7);
        const touched = tx < 160 && ty < 240;
        this.list.push({
          us: (timeUs + d.getUint16(o, true) * 1000) >>> 0, keys: d.getUint16(o + 2, true) & 0xfff,
          cx: d.getInt8(o + 4), cy: d.getInt8(o + 5), tx: touched ? tx : TOUCH_NONE, ty: touched ? ty : TOUCH_NONE,
        });
      }
      return count;
    }
    // the state at time us (times asked do not go back)
    at(us) {
      while (this.i < this.list.length && this.list[this.i].us <= us) this.now = this.list[this.i++];
      return this.now;
    }
  }

  // screens: { outW, outH, topX, topY, botX, botY } (1x pixels; x < 0: not shown)
  function layout(s) {
    const l = { strips: false, ox: 0, oy: 0, botShown: s.botX >= 0, botX: s.botX, botY: s.botY };
    if (s.botX >= 40 && s.outW - (s.botX + 320) >= 40) { l.strips = true; l.ox = s.botX; l.oy = s.botY; return l; }
    const sx = s.topX >= 0 ? s.topX : s.botX, sy = s.topX >= 0 ? s.topY : s.botY;
    l.ox = sx + 6; l.oy = sy + 240 - 6 - PANEL_H;
    return l;
  }

  const SOFT = [0xc3 / 255, 0xc9 / 255, 0xd0 / 255, 0.92];
  const ACCENT = [1, 0x4d / 255, 0x4f / 255, 1];
  const DARK = [0x0e / 255, 0x10 / 255, 0x14 / 255, 0.62];
  const HALO = [0x0e / 255, 0x10 / 255, 0x14 / 255, 0.55];
  const NONE = [0, 0, 0, 0];
  const f = Math.fround;

  function shapes(l, st) {
    const o = [], k = st.keys, touched = st.tx !== TOUCH_NONE;
    const put = (cross, cx, cy, hx, hy, r, fill, stroke, sw) => o.push({ cross, cx, cy, hx, hy, r, fill, stroke, sw });
    const button = (cx, cy, hx, hy, r, held, sw) => put(false, cx, cy, hx, hy, r, held ? ACCENT : NONE, held ? ACCENT : SOFT, sw);
    const dpad = (cx, cy, u, sw) => {
      const h = u * 0.5;
      if (k & KEY.UP) put(false, cx, cy - u, h, h, 1, ACCENT, NONE, 0);
      if (k & KEY.DOWN) put(false, cx, cy + u, h, h, 1, ACCENT, NONE, 0);
      if (k & KEY.LEFT) put(false, cx - u, cy, h, h, 1, ACCENT, NONE, 0);
      if (k & KEY.RIGHT) put(false, cx + u, cy, h, h, 1, ACCENT, NONE, 0);
      put(true, cx, cy, u * 1.5, h, 1.5, NONE, SOFT, sw);
    };
    const stick = (cx, cy, ring, dot, travel, sw) => {
      put(false, cx, cy, ring, ring, ring, NONE, SOFT, sw);
      put(false, cx + f(st.cx * travel / 127), cy + f(-st.cy * travel / 127), dot, dot, dot, SOFT, NONE, 0);
    };
    const abxy = (cx, cy, d, r, sw) => {
      button(cx, cy - d, r, r, r, k & KEY.X, sw);
      button(cx - d, cy, r, r, r, k & KEY.Y, sw);
      button(cx + d, cy, r, r, r, k & KEY.A, sw);
      button(cx, cy + d, r, r, r, k & KEY.B, sw);
    };
    if (l.strips) {
      const sw = 1.3, y0 = l.oy, lx = l.ox - 20, rx = l.ox + 340;
      button(lx, y0 + 14, 14, 4, 4, k & KEY.L, sw);
      stick(lx, y0 + 62, 13, 5, 8, sw); // as on the console: the stick above the D-pad
      dpad(lx, y0 + 118, 9, sw);
      button(lx, y0 + 170, 10, 3.5, 3.5, k & KEY.SELECT, sw);
      button(rx, y0 + 14, 14, 4, 4, k & KEY.R, sw);
      abxy(rx, y0 + 70, 11, 5.5, sw);
      button(rx, y0 + 170, 10, 3.5, 3.5, k & KEY.START, sw);
    } else {
      const sw = 1.1, x0 = l.ox, y0 = l.oy;
      put(false, x0 + PANEL_W / 2, y0 + PANEL_H / 2, PANEL_W / 2, PANEL_H / 2, 8, DARK, NONE, 0);
      button(x0 + 19, y0 + 7, 13, 3, 3, k & KEY.L, sw);
      button(x0 + 97, y0 + 7, 13, 3, 3, k & KEY.R, sw);
      stick(x0 + 19, y0 + 25, 8, 3.2, 5, sw);
      dpad(x0 + 19, y0 + 47, 6, sw);
      if (!l.botShown) {
        put(false, x0 + 58, y0 + 27, 16, 12, 2, NONE, SOFT, sw);
        if (touched) put(false, x0 + 42 + f((st.tx * 2 + 1) / 10), y0 + 15 + f(st.ty / 10), 2, 2, 2, ACCENT, NONE, 0);
      }
      button(x0 + 51, y0 + 48, 6, 2.2, 2.2, k & KEY.SELECT, sw);
      button(x0 + 65, y0 + 48, 6, 2.2, 2.2, k & KEY.START, sw);
      abxy(x0 + 97, y0 + 31, 9, 4, sw);
    }
    if (l.botShown && touched) {
      const x = l.botX + st.tx * 2 + 1, y = l.botY + st.ty + 0.5;
      put(false, x, y, 8.5, 8.5, 8.5, NONE, HALO, 3);
      put(false, x, y, 7, 7, 7, NONE, ACCENT, 2);
      put(false, x, y, 2, 2, 2, ACCENT, NONE, 0);
    }
    return o;
  }

  function sdBox(px, py, hx, hy, r) {
    const qx = Math.abs(px) - hx + r, qy = Math.abs(py) - hy + r;
    const ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, inn = qx > qy ? qx : qy;
    return Math.sqrt(ox * ox + oy * oy) + (inn < 0 ? inn : 0) - r;
  }
  function sd(s, x, y) {
    const px = x - s.cx, py = y - s.cy;
    if (s.cross) return Math.min(sdBox(px, py, s.hy, s.hx, s.r), sdBox(px, py, s.hx, s.hy, s.r));
    return sdBox(px, py, s.hx, s.hy, s.r);
  }
  const cover = (d, px) => { const c = 0.5 - d / px; return c < 0 ? 0 : c > 1 ? 1 : c; };
  function over(o, c, a) {
    const k = c[3] * a;
    if (k <= 0) return;
    o[0] = c[0] * k + o[0] * (1 - k); o[1] = c[1] * k + o[1] * (1 - k); o[2] = c[2] * k + o[2] * (1 - k); o[3] = k + o[3] * (1 - k);
  }
  function shade(sh, x, y, px, o) {
    o[0] = o[1] = o[2] = o[3] = 0;
    for (const s of sh) {
      const ex = s.hx + px + 1, ey = (s.cross ? s.hx : s.hy) + px + 1;
      if (Math.abs(x - s.cx) > ex || Math.abs(y - s.cy) > ey) continue;
      const d = sd(s, x, y);
      if (s.fill[3] > 0) over(o, s.fill, cover(d, px));
      if (s.stroke[3] > 0 && s.sw > 0) { const h = s.sw * 0.5; over(o, s.stroke, cover(Math.abs(d + h) - h, px)); }
    }
  }

  function clip(r, w, h) {
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > w) r.w = w - r.x;
    if (r.y + r.h > h) r.h = h - r.y;
    return r.w > 0 && r.h > 0;
  }
  function rects(l, st, outW, outH) {
    const r = [];
    if (l.strips) { r.push({ x: l.ox - 40, y: l.oy, w: 40, h: 240 }, { x: l.ox + 320, y: l.oy, w: 40, h: 240 }); }
    else r.push({ x: l.ox, y: l.oy, w: PANEL_W, h: PANEL_H });
    if (l.botShown && st.tx !== TOUCH_NONE) {
      const x = l.botX + st.tx * 2 + 1, y = l.botY + st.ty;
      r.push({ x: x - TOUCH_BOX / 2, y: y - TOUCH_BOX / 2, w: TOUCH_BOX, h: TOUCH_BOX });
    }
    return r.filter((x) => clip(x, outW, outH));
  }

  // The overlay for a state as patches at a scale: [{ x, y, w, h, data: Uint8ClampedArray rgba (straight alpha) }]
  // in output pixels, to drawImage over the picture (source-over).
  function patches(l, st, outW, outH, scale) {
    const sh = shapes(l, st), px = 1 / scale, c = [0, 0, 0, 0], res = [];
    for (const r of rects(l, st, outW, outH)) {
      const w = r.w * scale, h = r.h * scale, data = new Uint8ClampedArray(w * h * 4);
      for (let y = 0; y < h; y++)
        for (let x = 0; x < w; x++) {
          shade(sh, r.x + (x + 0.5) * px, r.y + (y + 0.5) * px, px, c);
          if (c[3] <= 0) continue;
          const o = (y * w + x) * 4;
          data[o] = c[0] / c[3] * 255 + 0.5; data[o + 1] = c[1] / c[3] * 255 + 0.5; data[o + 2] = c[2] / c[3] * 255 + 0.5;
          data[o + 3] = c[3] * 255 + 0.5;
        }
      res.push({ x: r.x * scale, y: r.y * scale, w, h, data });
    }
    return res;
  }

  // Over an RGB(A) picture at scale 1, the way r3dconv draws (for the comparison with it).
  function drawRgba(rgba, w, h, l, st) {
    const sh = shapes(l, st), c = [0, 0, 0, 0];
    for (const r of rects(l, st, w, h))
      for (let y = r.y; y < r.y + r.h; y++)
        for (let x = r.x; x < r.x + r.w; x++) {
          shade(sh, x + 0.5, y + 0.5, 1, c);
          if (c[3] <= 0) continue;
          const o = (y * w + x) * 4;
          for (let k = 0; k < 3; k++) {
            const v = (c[k] + rgba[o + k] / 255 * (1 - c[3])) * 255 + 0.5;
            rgba[o + k] = v < 0 ? 0 : v > 255 ? 255 : v;
          }
        }
  }

  const same = (a, b) => a && b && a.keys === b.keys && a.cx === b.cx && a.cy === b.cy && a.tx === b.tx && a.ty === b.ty;

  // Where the screens are in a view of web/app.js (viewFor) or as recorded.
  function screensFor(hdr, layoutName) {
    const by = hdr.both ? 240 : 0;
    if (layoutName === 'top' && !hdr.bottomOnly && (hdr.both || hdr.bottomOnly)) return { outW: 400, outH: 240, topX: 0, topY: 0, botX: -1, botY: -1 };
    if (layoutName === 'bottom' && (hdr.both || hdr.bottomOnly)) return { outW: 320, outH: 240, topX: -1, topY: -1, botX: 0, botY: 0 };
    if (layoutName === 'side' && hdr.both) return { outW: 720, outH: 240, topX: 0, topY: 0, botX: 400, botY: 0 };
    return {
      outW: 400, outH: hdr.height, topX: hdr.bottomOnly ? -1 : 0, topY: 0,
      botX: hdr.both || hdr.bottomOnly ? 40 : -1, botY: hdr.both || hdr.bottomOnly ? by : -1,
    };
  }

  root.R3DInputs = { InputTrack, layout, patches, drawRgba, screensFor, same, TOUCH_NONE, KEY };
  if (typeof module !== 'undefined') module.exports = root.R3DInputs;
})(typeof globalThis !== 'undefined' ? globalThis : this);
