/* SEA (Dani Biro, MIT) CBR encoder, 3-bit residuals, 4-bit scale factors, 20-frame slices, mono -- a port of sea-codec
 * 0.8.0's encoder_cbr/encoder_base/lms/qt/dqt for PATCHWERK. Output: the chunk stream only (no file header). */
(function (root) {
  var SF = [1, 6, 21, 48, 90, 150, 232, 337, 469, 630, 823, 1051, 1315, 1618, 1963, 2352];
  var REC = [65536, 10922, 3120, 1365, 728, 436, 282, 194, 139, 104, 79, 62, 49, 40, 33, 27];
  var DQV = [0.75, 2.5, 4.5, 7]; /* x scale factor, rounded: */
  var DQT = [];
  for (var s = 0; s < 16; s++) { var row = []; for (var q = 0; q < 4; q++) { var v = Math.round(SF[s] * DQV[q]); row.push(v, -v); } DQT.push(row); }
  /* quantiser: scaled residual -8..8 -> code 0..7 */
  var QT = [7, 7, 7, 5, 5, 3, 3, 1, 0, 0, 2, 2, 4, 4, 6, 6, 6];
  var SLICE = 20, FPC = 500, MAXRANK = Infinity;
  function sign(x) { return x > 0 ? 1 : x < 0 ? -1 : 0; }
  function clamp16(v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
  function seaDiv(v, r) { var n = Math.floor((v * r + 32768) / 65536); return n + (sign(v) - sign(n)); }
  function mk() { return { h: [0, 0, 0, 0], w: [0, 0, -8192, 16384] }; }
  function cp(d, s) { for (var i = 0; i < 4; i++) { d.h[i] = s.h[i]; d.w[i] = s.w[i]; } }
  function pred(l) {
    var sum = 0;
    for (var i = 0; i < 4; i++) sum += l.w[i] * l.h[i];
    if (sum > 2147483647 || sum < -2147483648) return null;
    return Math.floor(sum / 8192);
  }
  function upd(l, sample, res) {
    var delta = res >> 4;
    for (var i = 0; i < 4; i++) l.w[i] += l.h[i] < 0 ? -delta : delta;
    l.h[0] = l.h[1]; l.h[1] = l.h[2]; l.h[2] = l.h[3]; l.h[3] = sample;
  }
  function safe(l, res) {
    var d = res >> 4;
    for (var i = 0; i < 4; i++) { var w = l.w[i] + (l.h[i] < 0 ? -d : d); if (w > 2147483647 || w < -2147483648) return false; }
    return true;
  }
  function penalty(l) {
    var sum = 0;
    for (var i = 0; i < 4; i++) sum += l.w[i] * l.w[i];
    var p = Math.floor(sum / 262144) - 0x8ff;
    return p > 0 ? p * p : 0;
  }
  function trial(x, off, n, sf, lms, best, res) {
    var rank = 0, rec = REC[sf], dq = DQT[sf];
    for (var i = 0; i < n; i++) {
      var smp = x[off + i], p = pred(lms);
      if (p === null) return Infinity;
      var sc = seaDiv(smp - p, rec); if (sc > 8) sc = 8; else if (sc < -8) sc = -8;
      var qn = QT[sc + 8], dqv = dq[qn], re = clamp16(p + dqv), e = smp - re;
      rank += e * e + penalty(lms);
      if (rank > best) break;
      if (!safe(lms, dqv)) return Infinity;
      upd(lms, re, dqv);
      res[i] = qn;
    }
    return rank;
  }
  /* pcm: Int16Array. Pads with zeros to a multiple of 20 frames. Returns {data: Uint8Array, frames}. */
  function encode(pcm, fpc) {
    fpc = fpc || FPC;
    var frames = Math.ceil(pcm.length / SLICE) * SLICE, x = new Int16Array(frames); x.set(pcm);
    var out = [], lms = mk(), prev = 0, cur = mk(), tmp = mk(), bestL = mk(), res = new Uint8Array(SLICE), bres = new Uint8Array(SLICE);
    for (var c0 = 0; c0 < frames; c0 += fpc) {
      var n = Math.min(fpc, frames - c0), nsl = n / SLICE;
      /* the chunk's start state goes out as 16-bit words: round it the same way, so the decoder starts where the encoder did */
      for (var i = 0; i < 4; i++) { lms.w[i] = (lms.w[i] << 16) >> 16; lms.h[i] = (lms.h[i] << 16) >> 16; }
      var sfs = new Uint8Array(nsl), codes = new Uint8Array(n);
      var start = new Int32Array(8);
      for (i = 0; i < 4; i++) { start[i] = lms.h[i]; start[4 + i] = lms.w[i]; }
      for (var k = 0; k < nsl; k++) {
        var best = Infinity, bsf = 0, off = c0 + k * SLICE;
        for (var si = 0; si < 16; si++) {
          var sf = (si + prev) % 16;
          cp(tmp, lms);
          var r = trial(x, off, SLICE, sf, tmp, best, res);
          if (r < best) { best = r; bres.set(res); cp(bestL, tmp); bsf = sf; }
        }
        prev = bsf; cp(lms, bestL); sfs[k] = bsf; codes.set(bres, k * SLICE);
      }
      /* serialise: header, LMS (history x4 then weights x4, 16-bit LE), scale factors (4 bits), residuals (3 bits) */
      var sfb = Math.ceil(nsl * 4 / 8), rb = Math.ceil(n * 3 / 8), o = new Uint8Array(4 + 16 + sfb + rb), p = 0;
      o[p++] = 1; o[p++] = (4 << 4) | 3; o[p++] = SLICE; o[p++] = 0x5A;
      for (i = 0; i < 8; i++) { o[p++] = start[i] & 255; o[p++] = (start[i] >> 8) & 255; }
      var acc = 0, nb = 0;
      function put(v, b) { acc = (acc << b) | v; nb += b; while (nb >= 8) { o[p++] = (acc >> (nb - 8)) & 255; nb -= 8; acc &= (1 << nb) - 1; } }
      function flush() { if (nb) { o[p++] = (acc << (8 - nb)) & 255; acc = 0; nb = 0; } }
      for (k = 0; k < nsl; k++) put(sfs[k], 4);
      flush();
      for (k = 0; k < n; k++) put(codes[k], 3);
      flush();
      out.push(o);
    }
    var total = 0; out.forEach(function (a) { total += a.length; });
    total = (total + 3) & ~3;   /* (padded to whole words, as core/pw_sea.inc stores it) */
    var data = new Uint8Array(total), q = 0; out.forEach(function (a) { data.set(a, q); q += a.length; });
    return { data: data, frames: frames };
  }
  /* bytes of a stream of `frames` (a multiple of 20) in chunks of fpc frames */
  function size(frames, fpc) {
    fpc = fpc || FPC; var t = 0;
    for (var c = 0; c < frames; c += fpc) { var n = Math.min(fpc, frames - c); t += 20 + Math.ceil(n / 40) + Math.ceil(n * 3 / 8); }
    return (t + 3) & ~3;
  }

  /* the decoder (the same arithmetic as core/pw_sea.inc): stream -> Int16Array of `frames` */
  var FULL = 221;
  function decode(stream, frames) {
    var out = new Int16Array(frames), h = [0, 0, 0, 0], w = [0, 0, 0, 0];
    for (var c0 = 0, ci = 0; c0 < frames; c0 += FPC, ci++) {
      var n = Math.min(FPC, frames - c0), p = ci * FULL, i;
      for (i = 0; i < 4; i++) {
        h[i] = (stream[p + 4 + 2 * i] | (stream[p + 5 + 2 * i] << 8)) << 16 >> 16;
        w[i] = (stream[p + 12 + 2 * i] | (stream[p + 13 + 2 * i] << 8)) << 16 >> 16;
      }
      var sf = p + 20, rs = sf + Math.ceil(n / 40), bit = 0;
      for (var k = 0; k < n / SLICE; k++) {
        var dq = DQT[(stream[sf + (k >> 1)] >> ((k & 1) ? 0 : 4)) & 15];
        for (var j = 0; j < SLICE; j++, bit += 3) {
          var by = rs + (bit >> 3), sh = bit & 7;
          var q = (((stream[by] << 8) | (stream[by + 1] || 0)) >> (13 - sh)) & 7;
          var pr = (Math.imul(w[0], h[0]) + Math.imul(w[1], h[1]) + Math.imul(w[2], h[2]) + Math.imul(w[3], h[3])) >> 13;
          var d = dq[q], v = clamp16(pr + d), dl = d >> 4;
          for (i = 0; i < 4; i++) w[i] += h[i] < 0 ? -dl : dl;
          h[0] = h[1]; h[1] = h[2]; h[2] = h[3]; h[3] = v;
          out[c0 + k * SLICE + j] = v;
        }
      }
    }
    return out;
  }
  /* The default way a sound goes to the FM-1: the silence cut off both ends (the tail where the level, in 10 ms
   * windows, falls 50 dB under the peak; the start a window before the first sound), an 8 ms fade out, then SEA.
   * Returns {data: Int16Array (what the FM-1 will play), bytes: Uint8Array (the stream), frames, start}. */
  function trim(pcm, rate) {
    var pk = 0, i, hop = Math.max(1, Math.round(rate * 0.010));
    for (i = 0; i < pcm.length; i++) { var a = pcm[i] < 0 ? -pcm[i] : pcm[i]; if (a > pk) pk = a; }
    if (!pk) return { start: 0, end: Math.min(pcm.length, 20) };
    var thr = pk * Math.pow(10, -50 / 20), first = -1, last = -1;
    for (var b = 0, wi = 0; b < pcm.length; b += hop, wi++) {
      var s2 = 0, e = Math.min(pcm.length, b + hop);
      for (i = b; i < e; i++) s2 += pcm[i] * pcm[i];
      if (Math.sqrt(s2 / hop) > thr) { if (first < 0) first = wi; last = wi; }
    }
    if (first < 0) return { start: 0, end: pcm.length };
    return { start: Math.max(0, first * hop - hop), end: Math.min(pcm.length, (last + 1) * hop) };
  }
  /* the sound with its silence cut off both ends and an 8 ms fade out: {data, start} */
  function cut(pcm, rate) {
    var t = trim(pcm, rate), y = new Int16Array(pcm.subarray(t.start, t.end)), f = Math.min(y.length, Math.round(rate * 0.008));
    for (var i = 0; i < f; i++) y[y.length - f + i] = Math.round(y[y.length - f + i] * (1 - i / Math.max(1, f - 1)));
    return { data: y, start: t.start };
  }
  /* doTrim false: the whole sound is packed as it is */
  function compress(pcm, rate, doTrim) {
    var c = doTrim === false ? { data: new Int16Array(pcm), start: 0 } : cut(pcm, rate), y = c.data;
    if (y.length < 20) { var z = new Int16Array(20); z.set(y); y = z; }
    var r = encode(y, FPC);
    return { bytes: r.data, frames: r.frames, start: c.start, data: decode(r.data, r.frames) };
  }

  var api = { encode: encode, decode: decode, trim: trim, cut: cut, compress: compress, size: size, FPC: FPC, SLICE: SLICE };
  if (typeof module !== 'undefined') module.exports = api; else root.SeaEnc = api;
})(this);
