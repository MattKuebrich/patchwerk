// Patch-map view: screenshots plus a from-scratch voice built with SELECT/SAVE only.
import fs from 'fs'; import zlib from 'zlib';
const { instance } = await WebAssembly.instantiate(fs.readFileSync(new URL('../web/build/patchwerk.wasm', import.meta.url)), {});
const e = instance.exports; let t = 0;
e.pw_web_flash_format(); e.pw_web_init(-1);
const tick = (ms = 40) => { for (let i = 0; i < ms / 2.9; i++) e.pw_web_render(); t += ms; e.pw_web_control(t); };
const B = { ADD: 2, PATCH: 3, EDIT: 6, SHIFT: 7, HOME: 8, SAVE: 9, PATCHES: 10, PLAY: 12, REC: 13 };
const press = (b) => { e.pw_web_set_buttons(1 << b); tick(); e.pw_web_set_buttons(0); tick(); };
const enc = (i, d) => { for (let k = 0; k < Math.abs(d); k++) { e.pw_web_encoder(i, Math.sign(d)); tick(); } };
const json = () => JSON.parse(new TextDecoder().decode(new Uint8Array(e.memory.buffer, e.pw_web_buf(), e.pw_web_patch_json())));
const shots = [];
function png(name) {
  const fb = new Uint16Array(e.memory.buffer, e.pw_web_fb(), 57600), raw = Buffer.alloc(240 * 721);
  for (let y = 0; y < 240; y++) for (let x = 0; x < 240; x++) { const v = fb[y * 240 + x], o = y * 721 + 1 + x * 3; raw[o] = (v >> 11) << 3; raw[o + 1] = ((v >> 5) & 63) << 2; raw[o + 2] = (v & 31) << 3; }
  const crc = (b) => { let c = ~0; for (const x of b) { c ^= x; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; } return ~c >>> 0; };
  const ch = (ty, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(ty), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
  const h = Buffer.alloc(13); h.writeUInt32BE(240, 0); h.writeUInt32BE(240, 4); h[8] = 8; h[9] = 2;
  fs.writeFileSync(new URL(`lcd/${name}.png`, import.meta.url), Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]), ch('IHDR', h), ch('IDAT', zlib.deflateSync(raw)), ch('IEND', Buffer.alloc(0))]));
  shots.push(name);
}
for (let i = 0; i < 60; i++) tick();
press(B.HOME); png('pv1_basic');
e.pw_web_factory(1); tick(); press(B.PLAY); for (let i = 0; i < 20; i++) tick(); png('pv2_berlin'); press(B.PLAY);
// from scratch: new patch with an output
press(B.PATCHES); enc(1, -30); press(B.EDIT); press(B.HOME);
press(B.ADD); enc(1, -40); press(B.SAVE);          // VCO, cursor parks on its first input
enc(0, 6);                                           // V/OCT FM PWM SYNC SIN TRI -> SAW
png('pv3_on_saw');
press(B.ADD); enc(1, -40); enc(1, 5); press(B.SAVE); // VCF (6th in the add list) while on SAW -> carrying SAW, cursor VCF IN
png('pv4_carry_into_vcf');
press(B.SAVE); png('pv5_plugged');
enc(0, 3);                                           // IN FREQ RES -> LP
press(B.SAVE);                                       // pick up LP, hops to the next input
png('pv6_carry_lp');
for (let i = 0; i < 5; i++) { const j = json(); if (j.cables.length) break; }
enc(0, 1); press(B.SAVE);                            // next input after the hop is OUT L, plug
let j = json(); console.log('cables:', j.cables.map(c => c.slice(1).join('-')).join(' '), 'modules', j.modules.map(m => m.label).join(','));
png('pv7_done');
press(B.REC); console.log('after REC on cursor jack, cables', json().cables.length); png('pv8_unplug');
console.log(shots.join(' '));
