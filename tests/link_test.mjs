// SPDX-License-Identifier: GPL-3.0-only
// The live link (app/core/pw_link.c) end to end: two copies of the emulator's engine, one the FM-1 (device),
// one the web page (host), joined by a simulated USB-MIDI cable. Edits on either side, at once, frames lost,
// a full FM-1: the two patches must come out the same.
import { readFileSync } from 'node:fs';
const bytes = readFileSync(new URL('../web/build/patchwerk.wasm', import.meta.url));
const devBytes = readFileSync(new URL('../web/build/patchwerk-fm1caps.wasm', import.meta.url));   // the FM-1's limits
const dec = new TextDecoder();
async function engine(role) {
  const M = (await WebAssembly.instantiate(role === 0 ? devBytes : bytes, {})).instance.exports;
  M.pw_web_flash_format(); M.pw_web_init(-1); M.pw_web_link_init(role);
  return M;
}
const dev = await engine(0), host = await engine(1);
let now = 1000, failures = 0, dropNext = 0, dropped = 0, dropHost = 0, dropAck = 0, frames = [0, 0];
const check = (ok, msg) => { if (!ok) { failures++; console.log('FAIL', msg); } };
function deliver(from, to, idx) {
  for (let n; (n = from.pw_web_link_tx()) > 0;) {
    const f = new Uint8Array(from.memory.buffer, from.pw_web_link_out(), n).slice();
    frames[idx]++;
    if (idx === 0 && dropNext > 0 && f[4] === 0x55 && f.length > 40) { dropNext--; dropped++; continue; }   // a lost device frame
    if (idx === 1 && dropHost > 0 && f[4] === 0x55 && f.length > 12) { dropHost--; continue; }             // a lost page frame
    if (idx === 0 && dropAck > 0 && f[4] === 0x4B) { dropAck--; continue; }                                 // a lost answer
    new Uint8Array(to.memory.buffer, to.pw_web_buf(), n).set(f);
    to.pw_web_link_rx(n, now);
  }
}
function run(ms) {
  for (let t = 0; t < ms; t += 5) {
    now += 5;
    dev.pw_web_link_poll(now); host.pw_web_link_poll(now);
    deliver(dev, host, 0); deliver(host, dev, 1);
  }
}
function state(M) {
  const n = M.pw_web_patch_json(), p = JSON.parse(dec.decode(new Uint8Array(M.memory.buffer, M.pw_web_buf(), n)));
  const mods = p.modules.map((m) => [m.slot, m.type, m.x, m.y, m.p.map((v) => +v.toFixed(5)), m.steps || null, m.gates ?? null]);
  const cables = p.cables.map((c) => c.slice(1).join(',')).sort();
  return JSON.stringify({ name: p.name, bpm: +p.bpm.toFixed(3), playing: p.playing, mods, cables });
}
const same = (what) => {
  const a = state(dev), b = state(host);
  let i = 0; while (i < a.length && a[i] === b[i]) i++;
  check(a === b, `${what}: patches differ at ${i}\n  dev  ...${a.slice(Math.max(0, i - 150), i + 150)}\n  host ...${b.slice(Math.max(0, i - 150), i + 150)}`);
  return a === b;
};

// 1. hello: the page takes the FM-1's patch
dev.pw_web_factory(1); dev.pw_web_set_bpm(97);
host.pw_web_link_hello(now);
run(400);
check(host.pw_web_link_active() && dev.pw_web_link_active(), 'link up');
check((host.pw_web_link_cap() & 255) === 24, `device capacity ${host.pw_web_link_cap() & 255}`);
same('after hello');
// 2. the page edits: the FM-1 follows
const vco = host.pw_web_add(0, 900, 100), vcf = 2;
host.pw_web_connect(vco, 2, 5, 0); host.pw_web_set_param(vco, 0, -12); host.pw_web_move(vco, 1234, 567);
{ const c = host.pw_web_connect(vco, 0, 5, 1); host.pw_web_cable_amount(c, -40); }
host.pw_web_set_seq_step(0, 3, 7, 1); host.pw_web_set_bpm(133); host.pw_web_play(1);
run(300);
same('after page edits');
// 3. the FM-1 edits: the page follows
dev.pw_web_remove(vcf); dev.pw_web_set_param(vco, 1, 33); dev.pw_web_play(0);
run(300);
same('after device edits');
// 4. both at once, a lot
let seed = 7; const rnd = (n) => { seed = (seed * 1103515245 + 12345) >>> 0; return (seed >>> 8) % n; };
for (let round = 0; round < 40; round++) {
  for (const M of [dev, host]) {
    const k = rnd(6), n = M.pw_web_patch_json(), p = JSON.parse(dec.decode(new Uint8Array(M.memory.buffer, M.pw_web_buf(), n)));
    const ms = p.modules;
    if (k === 0 || !ms.length) M.pw_web_add(rnd(45), rnd(2000), rnd(2000));
    else if (k === 1 && ms.length > 3) M.pw_web_remove(ms[rnd(ms.length)].slot);
    else if (k === 2) { const a = ms[rnd(ms.length)], b = ms[rnd(ms.length)]; M.pw_web_connect(a.slot, rnd(3), b.slot, rnd(3)); }
    else if (k === 3 && p.cables.length) M.pw_web_disconnect(p.cables[rnd(p.cables.length)][0]);
    else { const a = ms[rnd(ms.length)]; M.pw_web_set_param(a.slot, rnd(4), rnd(1000) / 10); }
  }
  run(rnd(3) * 15);
}
run(1500);
same('after 40 rounds of edits on both sides');
// 5. a device frame lost: the page notices and starts over
dropNext = 1;
dev.pw_web_factory(3);
run(1500);
check(dropped === 1 && host.pw_web_link_stat(5) >= 1, `gap noticed (dropped ${dropped}, gaps ${host.pw_web_link_stat(5)})`);
same('after a lost frame');
// 5b. a page frame lost, then an answer lost: sent again, applied once
dropHost = 1; host.pw_web_set_param(0, 0, 0.25); run(800);
dropAck = 1; host.pw_web_add(2, 5, 5); run(800);
check(host.pw_web_link_stat(4) >= 2, `resends ${host.pw_web_link_stat(4)}`);
same('after lost page frames and answers');
// 6. more modules than the FM-1 holds: the extra ones come back off the page
host.pw_web_clear();
for (let i = 0; i < 30; i++) host.pw_web_add(1, i * 10, 0);
run(1500);
const nd = JSON.parse(dec.decode(new Uint8Array(dev.memory.buffer, dev.pw_web_buf(), dev.pw_web_patch_json()))).modules.length;
check(nd === 18, `device modules ${nd}`);   // (the FM-1 has 72 output buffers: 18 modules of 4 outputs)
same('after overfilling the FM-1');
// 6b. more cables than the FM-1 holds (64)
host.pw_web_clear();
for (let i = 0; i < 6; i++) host.pw_web_add(18, i * 10, 0);      // CV mixers: 4 inputs, 2 outputs each
for (let a = 0; a < 6; a++) for (let o = 0; o < 2; o++) for (let b = 0; b < 6; b++) for (let k = 0; k < 4; k++) host.pw_web_connect(a, o, b, k);
run(3000);
const cd = JSON.parse(dec.decode(new Uint8Array(dev.memory.buffer, dev.pw_web_buf(), dev.pw_web_patch_json()))).cables.length;
check(cd === 64, `device cables ${cd}`);
same('after overfilling the cables');
// 7. the page goes away: the FM-1 notices; and back
run(6000 * 0 + 10);
const before = dev.pw_web_link_active();
for (let t = 0; t < 6000; t += 50) { now += 50; dev.pw_web_link_poll(now); dev.pw_web_link_tx(); }
check(before && !dev.pw_web_link_active(), 'device times out without the page');
host.pw_web_link_hello(now); run(800);
same('after reconnecting');
if (process.env.DBG) { const a = JSON.parse(state(dev)), b = JSON.parse(state(host)); console.log("dev", a.mods.length, a.cables.length, "host", b.mods.length, b.cables.length); console.log(JSON.stringify(a.mods.slice(20)), JSON.stringify(b.mods.slice(20))); }
console.log(`link: frames device->page ${frames[0]}, page->device ${frames[1]}, ops sent dev ${dev.pw_web_link_stat(2)} page ${host.pw_web_link_stat(2)}, resends ${host.pw_web_link_stat(4)}`);
console.log(failures ? `${failures} LINK FAILURES` : 'LINK TESTS PASSED');
process.exit(failures ? 1 : 0);
