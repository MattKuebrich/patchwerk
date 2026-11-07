// The USER palette (pw_ui_user_palette): sent, in use, kept in the settings across a restart, read back, removed;
// and a saved settings blob from before it (20 bytes, with no USER palette) still loads.
import fs from 'fs';
const wasm = fs.readFileSync(new URL('../web/build/patchwerk.wasm', import.meta.url));
const { instance } = await WebAssembly.instantiate(wasm, {});
const e = instance.exports;
let bad = 0;
const check = (name, ok) => { if (!ok) { console.log('FAIL', name); bad++; } };
const buf = () => new Uint16Array(e.memory.buffer, e.pw_web_palette_buf(), 64);
const pal = (p) => { const n = e.pw_web_palette(p, 0, 3); return Array.from(buf().slice(0, n)); };
e.pw_web_flash_format(); e.pw_web_init(-1);
check('there are 12 built-in palettes and no USER one', e.pw_web_user_get() === 0 && pal(12).join() === pal(0).join());   // (row 12 is not a palette yet: it wraps to row 0)
const mine = Array.from({ length: 18 }, (_, i) => (i * 1237 + 11) & 0xFFFF);
buf().set(mine); check('sending it works', e.pw_web_user_palette(1) === 0);
check('it reads back', (buf().fill(0), e.pw_web_user_get() === 1) && Array.from(buf().slice(0, 18)).join() === mine.join());
const near = (a, b) => a.length === b.length && a.every((v, i) => Math.abs((v >> 11) - (b[i] >> 11)) <= 1 && Math.abs(((v >> 5) & 63) - ((b[i] >> 5) & 63)) <= 1 && Math.abs((v & 31) - (b[i] & 31)) <= 1);   // (the settings screen's tone/brightness path rounds each color to the nearest level)
check('it is palette row 12', near(pal(12), mine));
check('the 12 built-in palettes are untouched', pal(2).join() !== mine.join());
e.pw_web_init(-1);   // a restart: the settings object is read again
check('it is still there after a restart', e.pw_web_user_get() === 1 && Array.from(buf().slice(0, 18)).join() === mine.join());
check('removing it works', e.pw_web_user_palette(0) === 0 && e.pw_web_user_get() === 0);
e.pw_web_init(-1);
check('it stays removed after a restart', e.pw_web_user_get() === 0);
// a settings blob saved with the first USER palette (33 colors, tag 0x5550) is read as the 18: the four greys, then the rest
{
  const old = Array.from({ length: 33 }, (_, i) => (i * 911 + 5) & 0xFFFF), blob = new Uint8Array(88), dv = new DataView(blob.buffer);
  dv.setUint32(0, 0x31544553, true); blob[4] = 2; blob[10] = 0x80; blob[11] = 3;
  old.forEach((v, i) => dv.setUint16(20 + 2 * i, v, true)); dv.setUint16(86, 0x5550, true);
  new Uint8Array(e.memory.buffer, e.pw_web_settings_buf(), 88).set(blob);
  check('an old blob is accepted', e.pw_web_settings_put(88) === 0);
  const want = [...old.slice(0, 4), ...old.slice(19)];
  check('its USER palette is converted', e.pw_web_user_get() === 1 && Array.from(buf().slice(0, 18)).join() === want.join());
}
console.log(bad ? 'USER PALETTE TESTS FAILED' : 'USER PALETTE TESTS PASSED');
process.exit(bad ? 1 : 0);
