// SPDX-License-Identifier: GPL-3.0-only
// web/sea.js (the page's SEA codec) against tests/sea_fixture.inc: the decoder gives the reference decoder's samples
// (sea-codec 0.8.0's c/sea.h), the encoder makes the fixture's stream from the sound it was made from, the sizes agree
// with core/pw_sea.inc's, and a round trip of a trimmed sound is close.   node tests/sea_test.mjs
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const SEA = require('../web/sea.js');
const inc = readFileSync(new URL('./sea_fixture.inc', import.meta.url), 'utf8');
const arr = (name) => inc.match(new RegExp(name + '\\[\\d+\\] = \\{([^}]*)\\}'))[1].split(',').map((s) => s.trim()).filter(Boolean).map(Number);
const stream = Uint8Array.from(arr('sea_fix_stream')), pcm = Int16Array.from(arr('sea_fix_pcm')), src = Int16Array.from(arr('sea_fix_src'));
let fails = 0;
const check = (ok, msg) => { if (!ok) { fails++; console.log('FAIL', msg); } };
const d = SEA.decode(stream, 3020);
let bad = 0; for (let i = 0; i < 3020; i++) bad += d[i] !== pcm[i];
check(bad === 0, `the decoder differs from the reference decoder in ${bad} of 3020 frames`);
const e = SEA.encode(src, 500);
check(e.frames === 3020 && e.data.length === stream.length && e.data.every((v, i) => v === stream[i]), 'the encoder does not reproduce the fixture stream');
check(SEA.size(3020, 500) === stream.length, `size() ${SEA.size(3020, 500)} vs ${stream.length}`);
const c = SEA.compress(new Int16Array(30000).map((_, i) => Math.round(Math.sin(i * 0.05) * 8000 * Math.exp(-i / 4000))), 44100);
check(c.frames % 20 === 0 && c.bytes.length === SEA.size(c.frames, 500) && c.frames < 30000, `compress: ${c.frames} frames, ${c.bytes.length} bytes`);
let num = 0, den = 0; const t = SEA.trim(new Int16Array(30000).map((_, i) => Math.round(Math.sin(i * 0.05) * 8000 * Math.exp(-i / 4000))), 44100);
check(t.end < 30000 && t.start === 0, `trim: ${t.start}..${t.end}`);
{   /* compress without trimming keeps the whole sound (to a multiple of 20); cut() alone is the trimmed sound with its fade */
  const snd = new Int16Array(30010).map((_, i) => Math.round(Math.sin(i * 0.05) * 8000 * Math.exp(-i / 4000)));
  const w = SEA.compress(snd, 44100, false), k = SEA.cut(snd, 44100);
  check(w.frames === 30020 && w.start === 0 && w.bytes.length === SEA.size(30020, 500), `compress, not trimmed: ${w.frames} frames`);
  check(k.data.length < 30010 && k.data[k.data.length - 1] === 0, `cut: ${k.data.length} frames`);
}
for (let i = 0; i < 3020; i++) { num += src[i] * src[i]; den += (src[i] - d[i]) ** 2; }
check(10 * Math.log10(num / den) > 25, `round trip SNR ${(10 * Math.log10(num / den)).toFixed(1)} dB`);
console.log(fails ? `${fails} SEA FAILURES` : 'SEA TESTS PASSED');
process.exit(fails ? 1 : 0);
