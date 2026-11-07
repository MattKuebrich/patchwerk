// The module page's pictures: one shot per module, put together in tests/lcd/edit_*.png (look at them).
import fs from 'fs'; import zlib from 'zlib';
const { instance } = await WebAssembly.instantiate(fs.readFileSync(new URL('../web/build/patchwerk.wasm', import.meta.url)), {});
const e = instance.exports; let t = 0;
e.pw_web_flash_format(); e.pw_web_init(-1);
const tick = (ms = 40) => { for (let i = 0; i < ms / 2.9; i++) e.pw_web_render(); t += ms; e.pw_web_control(t); };
const B = { EDIT: 6, GLO: 7, HOME: 8, PLAY: 12 };
const press = (b) => { e.pw_web_set_buttons(1 << b); tick(); e.pw_web_set_buttons(0); tick(); };
const enc = (i, d) => { for (let k = 0; k < Math.abs(d); k++) { e.pw_web_encoder(i, Math.sign(d)); tick(); } };
const types = JSON.parse(new TextDecoder().decode(new Uint8Array(e.memory.buffer, e.pw_web_buf(), e.pw_web_types_json()))).types;
const T = (name) => types.findIndex((x) => x.name === name);
for (let i = 0; i < 60; i++) tick();
const shots = [];
const grab = () => new Uint16Array(e.memory.buffer, e.pw_web_fb(), 57600).slice();
function shot(name, type, setup, page = 0) {
  e.pw_web_clear();
  const s = e.pw_web_add(T(type), 0, 0);
  if (setup) setup(s);
  press(B.HOME); e.pw_web_ui_select(s); tick(); press(B.EDIT);
  if (e.pw_web_ui_page() !== 2) press(B.EDIT);
  enc(1, page);   /* ALGORITHM turns the page on the module page */
  for (let i = 0; i < 6; i++) tick();
  shots.push([name, grab()]);
}
const P = (s, p, v) => e.pw_web_set_param(s, p, v);
shot('ENVELOPE', 'ENVELOPE', (s) => { const k = e.pw_web_add(T('KEYS'), 0, 0); e.pw_web_connect(k, 1, s, 0); e.pw_web_key(60, 100); });
shot('VCF 303', 'VCF', (s) => { P(s, 4, 2); P(s, 1, 0.8); });
shot('VCF MOOG p2', 'VCF', (s) => { P(s, 4, 1); P(s, 1, 0.5); P(s, 0, 600); }, 1);
shot('EQ', 'EQUALIZER', (s) => { P(s, 0, 6); P(s, 1, -9); P(s, 2, 4); P(s, 3, 800); });
shot('COMB', 'COMB FILTER', null);
shot('FORMANT', 'FORMANT OSC', (s) => P(s, 1, 1.5));
shot('FM p1', 'FM SYNTH', (s) => { const k = e.pw_web_add(T('KEYS'), 0, 0); e.pw_web_connect(k, 1, s, 1); P(s, 0, 1); P(s, 1, 0.4); P(s, 12, 7); P(s, 13, 0); e.pw_web_key(60, 100); });
shot('FM op 2', 'FM SYNTH', (s) => { P(s, 0, 4); P(s, 13, 6); P(s, 17, 1.5); const k = e.pw_web_add(T('KEYS'), 0, 0); e.pw_web_connect(k, 1, s, 1); e.pw_web_key(60, 100); }, 2);
shot('FM op 1', 'FM SYNTH', (s) => { P(s, 0, 4); }, 1);
shot('DRUM RACK', 'DRUM RACK', (s) => { P(s, 2, 1); e.pw_web_key(62, 100); }, 3);
{ /* a sound in bank slot 1: a decaying 220 Hz saw, a second */
  const n = 22050, ptr = e.pw_web_bank_alloc(0, n, 22050), d = new Int16Array(e.memory.buffer, ptr, n);
  for (let i = 0; i < n; i++) d[i] = Math.round(20000 * Math.exp(-i / 9000) * (((i * 220 / 22050) % 1) * 2 - 1));
  e.pw_web_bank_commit(0, n, 0, 0x52_49_44 /* "DIR" */ & 0, 0, 0, 0);
}
shot('SAMPLER', 'SAMPLER', (s) => { P(s, 0, 1); P(s, 4, 0.1); P(s, 5, 0.7); P(s, 6, 1); });
shot('SAMPLER RAM slices', 'SAMPLER', (s) => { P(s, 1, 2); P(s, 13, 1); P(s, 12, 8); }, 3);
shot('SAMPLER warp', 'SAMPLER', (s) => { P(s, 8, 1); P(s, 9, 0.5); P(s, 7, 1); P(s, 1, 3); }, 2);
shot('SAMPLER amp', 'SAMPLER', null, 4);
shot('MIDI IN', 'MIDI IN', null);
shot('TRIG SEQ', 'TRIG SEQ', (s) => { e.pw_web_play(1); });
shot('EUCLID', 'EUCLID', (s) => { e.pw_web_play(1); });
shot('SEQUENCER', 'SEQUENCER', (s) => { e.pw_web_play(1); });
shot('DRUM', 'DRUM SYNTH', (s) => { P(s, 5, 0.4); });
shot('STEREO MIX', 'MIXER', (s) => { P(s, 4, -0.6); P(s, 7, 0.8); });
shot('LFO', 'LFO', null);
shot('DELAY (pages)', 'DELAY', null);
shot('PITCH SHIFT', 'PITCH SHIFTER', null);
shot('STEREO p2', 'MIXER', null, 1);
shot('COMPRESSOR', 'COMPRESSOR', null);
shot('FM jacks', 'FM SYNTH', (s) => { const k = e.pw_web_add(T('KEYS'), 0, 0); e.pw_web_connect(k, 0, s, 0); e.pw_web_connect(k, 1, s, 1); });
press(B.EDIT); for (let i = 0; i < 4; i++) tick(); shots[shots.length - 1][1] = grab();
e.pw_web_play(0);
{ /* the performance page: three knobs put on it, variation 3 */
  e.pw_web_clear(); const v = e.pw_web_add(T('VCF'), 0, 0), d = e.pw_web_add(T('DELAY'), 0, 0), f = e.pw_web_add(T('FM SYNTH'), 0, 0);
  e.pw_web_perf_assign(v, 0); e.pw_web_perf_assign(v, 1); e.pw_web_perf_assign(d, 1); e.pw_web_perf_assign(f, 9);
  e.pw_web_var_select(2);
  press(B.HOME); e.pw_web_set_buttons(1 << 10); tick(); e.pw_web_set_buttons(0); tick(); for (let i = 0; i < 6; i++) tick();
  shots.push(['PERFORM', grab()]);
  e.pw_web_set_voices(4); enc(6, 1); for (let i = 0; i < 4; i++) tick();
  shots.push(['PATCHES voices', grab()]);
}
// montage, 4 across
const cols = 4, rows = Math.ceil(shots.length / cols), Wd = cols * 244, Hd = rows * 244;
const raw = Buffer.alloc(Hd * (Wd * 3 + 1), 40);
for (let y = 0; y < Hd; y++) raw[y * (Wd * 3 + 1)] = 0;
shots.forEach(([n, fb], k) => {
  const ox = (k % cols) * 244 + 2, oy = Math.floor(k / cols) * 244 + 2;
  for (let y = 0; y < 240; y++) for (let x = 0; x < 240; x++) { const v = fb[y * 240 + x], o = (oy + y) * (Wd * 3 + 1) + 1 + (ox + x) * 3; raw[o] = (v >> 11) << 3; raw[o + 1] = ((v >> 5) & 63) << 2; raw[o + 2] = (v & 31) << 3; }
});
const crc = (b) => { let c = ~0; for (const x of b) { c ^= x; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; } return ~c >>> 0; };
const ch = (ty, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(ty), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
const h = Buffer.alloc(13); h.writeUInt32BE(Wd, 0); h.writeUInt32BE(Hd, 4); h[8] = 8; h[9] = 2;
fs.mkdirSync(new URL('lcd/', import.meta.url), { recursive: true });
fs.writeFileSync(new URL('lcd/edit_pages.png', import.meta.url), Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]), ch('IHDR', h), ch('IDAT', zlib.deflateSync(raw)), ch('IEND', Buffer.alloc(0))]));
console.log('edit pages:', shots.map((s) => s[0]).join(', '));
