// MIDI clock out and program change (settings), and the settings as a backup (pw_ui_settings_get / put).
import fs from 'fs';
const wasm = fs.readFileSync(new URL('../web/build/patchwerk.wasm', import.meta.url));
const { instance } = await WebAssembly.instantiate(wasm, {});
const e = instance.exports;
let bad = 0, t = 1000;
const check = (name, ok) => { if (!ok) { console.log('FAIL', name); bad++; } };
const tick = (n = 40) => { for (let i = 0; i < n; i++) { e.pw_web_render(); t += 3; e.pw_web_control(t); } };
const sb = () => new Uint8Array(e.memory.buffer, e.pw_web_settings_buf(), 128);
const getS = () => { const n = e.pw_web_settings_get(); return n > 0 ? Array.from(sb().slice(0, n)) : []; };
const putS = (a) => { sb().set(a); return e.pw_web_settings_put(a.length); };
const taken = () => { const n = e.pw_web_midi_out_take(); const o = new Uint8Array(e.memory.buffer, e.pw_web_midi_out(), n * 3); const r = []; for (let i = 0; i < n; i++) r.push(o[i * 3]); return r; };
const json = () => { const n = e.pw_web_patch_json(); return new TextDecoder().decode(new Uint8Array(e.memory.buffer, e.pw_web_buf(), n)).replace(/"rev":\d+/, ''); };
e.pw_web_flash_format(); e.pw_web_init(-1); tick(10);

// settings backup: get, change, put, get
const s0 = getS();
check('the settings come out as 88 bytes with their magic', s0.length === 88 && s0[0] === 0x53 && s0[1] === 0x45 && s0[2] === 0x54 && s0[3] === 0x31);
const MSG = 9;   // (the blob: magic 0-3, pal, latch, autosave, edit_all, vel, msg, tone, bright ...)
{ const x = s0.slice(); x[MSG] |= 0x60; check('a changed settings blob goes in', putS(x) === 0); const y = getS(); check('and comes back (clock out and program change on)', (y[MSG] & 0x60) === 0x60); }
check('an older, shorter blob (12 bytes) goes in', putS(s0.slice(0, 12)) === 0);
check('garbage is refused', putS(Array(88).fill(7)) < 0);

// program change picks patches once the setting is on (re-enable: the old blob put cleared it)
{ const x = s0.slice(); x[MSG] |= 0x60; putS(x); }
e.pw_web_init(-1); tick(10);
const before = json();
e.pw_web_midi(0xC0, 10, 0); tick(10);
const after = json();
check('program 10 loads a factory patch', before !== after);
e.pw_web_midi(0xC0, 10, 0); tick(10);
check('the same program again gives the same patch', json() === after);
e.pw_web_midi(0xC0, 120, 0); tick(10);
check('a program past the patches changes nothing', json() === after);

// clock out: on while playing, start and stop, 24 a quarter
e.pw_web_play(0); tick(5); taken();
e.pw_web_play(1); tick(400);
const q = taken();
const clocks = q.filter((v) => v === 0xF8).length;
check('start is sent', q.includes(0xFA));
check('clocks are sent while playing', clocks > 20);
e.pw_web_play(0); tick(10);
check('stop is sent', taken().includes(0xFC));
// off: nothing
{ const x = getS(); x[MSG] &= ~0x20; putS(x); }
e.pw_web_play(1); tick(400);
check('with the setting off no clock goes out', !taken().includes(0xF8));
console.log(bad ? 'MIDI/SETTINGS TESTS FAILED' : 'MIDI/SETTINGS TESTS PASSED');
process.exit(bad ? 1 : 0);
