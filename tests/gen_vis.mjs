// Draws every module's module page (its visualization) with the firmware itself and writes the screens as raw RGB565
// files: node tests/gen_vis.mjs web/vis_raw   then   python3 tests/vis_png.py web/vis_raw web/vis
// Each module is patched with something to listen to: a saw into its audio jacks, a slow LFO into its CV jacks, a clock
// into its triggers and a sequencer's pitch into V/OCT, so the pictures move the way they would on the device.
import fs from 'fs';
const out = process.argv[2] || 'web/vis_raw';
fs.mkdirSync(out, { recursive: true });
const { instance } = await WebAssembly.instantiate(fs.readFileSync(new URL('../web/build/patchwerk.wasm', import.meta.url)), {});
const e = instance.exports; let t = 0;
e.pw_web_flash_format(); e.pw_web_init(-1);
const tick = (ms = 34) => { for (let i = 0; i < ms / 2.9; i++) e.pw_web_render(); t += ms; e.pw_web_control(t); };
for (let i = 0; i < 80; i++) tick();
const types = JSON.parse(new TextDecoder().decode(new Uint8Array(e.memory.buffer, e.pw_web_buf(), e.pw_web_types_json()))).types;
const id = (n) => types.findIndex((x) => x.name === n);
const press = (b) => { e.pw_web_set_buttons(1 << b); tick(); e.pw_web_set_buttons(0); tick(); };
const AUDIO = /^(IN|IN L|IN R|AUDIO|X|Y|A|B|SIDE|IN \d)$/, GATE = /^(CLOCK|CLK|TRIG|GATE|T\d|PING|DROP|FILL|EXT|ACC|ACCENT|STEP)$/, PITCH = /^(V\/OCT|PITCH|TRANSP)$/;
const manifest = [];
for (let i = 0; i < types.length; i++) {
  const ty = types[i];
  if (ty.hidden) continue;                             // the modules folded into others (v28): only old patches name them
  e.pw_web_clear(); e.pw_web_set_bpm(120);
  const slot = e.pw_web_add(i, 0, 0);
  const env = e.pw_web_add(id('ENVELOPE'), 0, 0), osc = e.pw_web_add(id('VCO'), 0, 0), lfo = e.pw_web_add(id('LFO'), 0, 0), clk = e.pw_web_add(id('CLOCK'), 0, 0), seq = e.pw_web_add(id('SEQUENCER'), 0, 0);
  e.pw_web_set_param(osc, 0, 1);
  const vo = types[id('VCO')].outs, sawPort = Math.max(0, vo.findIndex((n) => /SAW/.test(n)));
  e.pw_web_set_param(lfo, 0, 0.7); e.pw_web_connect(clk, 0, env, 0);
  ty.ins.forEach((n, k) => {
    if (AUDIO.test(n)) e.pw_web_connect(osc, sawPort, slot, k);
    else if (/^CV/.test(n) && !/^CV [AB]$/.test(n)) e.pw_web_connect(env, 0, slot, k);
    else if (PITCH.test(n)) e.pw_web_connect(seq, 0, slot, k);
    else if (GATE.test(n)) e.pw_web_connect(clk, 0, slot, k);
    else if (!/^(RESET|FREEZE|STOP|REC|PRESET|RETRIG|SYNC|TAP|LOCK)$/.test(n)) e.pw_web_connect(lfo, 0, slot, k);
  });
  if (ty.name === 'PIANO ROLL') { e.pw_web_set_param(slot, 0, 1); e.pw_web_roll_demo(slot); }
  e.pw_web_play(1);
  for (let k = 0; k < 90; k++) tick();
  e.pw_web_ui_select(slot);
  if (ty.name === 'KEYS' || ty.name === 'ARPEGGIATOR') for (const n of [60, 64, 67, 71]) e.pw_web_key(n, 100);
  if (ty.name === 'MIDI IN') [[1, 40], [74, 90], [71, 20], [7, 110]].forEach(([c, v]) => e.pw_web_midi(0xb0, c, v));
  press(8); press(6);                                 // HOME, then EDIT: the module page, picture first
  for (let k = 0; k < 60; k++) tick();
  const px = new Uint16Array(e.memory.buffer, e.pw_web_fb(), 57600).slice();
  fs.writeFileSync(`${out}/${i}.bin`, Buffer.from(px.buffer));
  manifest.push({ i, name: ty.name, vis: e.pw_web_has_vis(i) });
  if (ty.name === 'KEYS' || ty.name === 'ARPEGGIATOR') for (const n of [60, 64, 67, 71]) e.pw_web_key(n, 0);
  e.pw_web_play(0);
}
fs.writeFileSync(`${out}/manifest.json`, JSON.stringify(manifest));
console.log(`${manifest.length} modules, ${manifest.filter((m) => m.vis).length} with a picture`);
