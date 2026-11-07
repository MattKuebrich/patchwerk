// The screen's tile updates: pixels sent while notes play, and the picture equal to a full redraw.
import fs from 'fs';
const { instance } = await WebAssembly.instantiate(fs.readFileSync(new URL('../web/build/patchwerk.wasm', import.meta.url)), {});
const e = instance.exports; let t = 0;
e.pw_web_flash_format(); e.pw_web_init(-1);
const tick = (ms = 34) => { for (let i = 0; i < ms / 2.9; i++) e.pw_web_render(); t += ms; e.pw_web_control(t); };
for (let i = 0; i < 80; i++) tick();                 /* past the boot screen, onto the patch */
const press = (b) => { e.pw_web_set_buttons(1 << b); tick(); e.pw_web_set_buttons(0); tick(); };
press(8);                                              /* HOME */
for (let i = 0; i < 10; i++) tick();
const p0 = e.pw_web_blit_px();
for (let n = 0; n < 8; n++) {                          /* eight notes, held a quarter second each */
  e.pw_web_set_keys(1 << (7 + n)); for (let i = 0; i < 8; i++) tick();
  e.pw_web_set_keys(0); for (let i = 0; i < 4; i++) tick();
}
const frames = 8 * 12, px = e.pw_web_blit_px() - p0;
console.log(`while playing: ${(px / frames).toFixed(0)} px a frame, ${(px / frames / 57600 * 100).toFixed(1)}% of the screen`);
/* let the meters fall still: no pixel sent for ten frames */
for (let quiet = 0, last = e.pw_web_blit_px(), i = 0; quiet < 10 && i < 2000; i++) { tick(); const p = e.pw_web_blit_px(); quiet = p === last ? quiet + 1 : 0; last = p; }
/* (the engine stands still from here, so both pictures are of the same moment: the patch has things that flicker now and then, and a later frame would differ for that) */
e.pw_web_control(t);
const before = new Uint16Array(e.memory.buffer, e.pw_web_fb(), 57600).slice();
e.pw_web_redraw_all(); e.pw_web_control(t);
const after = new Uint16Array(e.memory.buffer, e.pw_web_fb(), 57600);
let diff = 0; for (let i = 0; i < 57600; i++) if (before[i] !== after[i]) diff++;
console.log(`tile updates vs a full redraw: ${diff} pixels differ`);
process.exit(diff ? 1 : 0);
