// Drive the firmware UI inside the wasm and save LCD screenshots as PNG.
import fs from 'fs'; import zlib from 'zlib';
const wasm = fs.readFileSync(new URL('../web/build/patchwerk.wasm', import.meta.url));
const { instance } = await WebAssembly.instantiate(wasm, {});
const e = instance.exports, mem = () => new Uint8Array(e.memory.buffer);
let t = 0;
e.pw_web_flash_format(); e.pw_web_init(-1);
const tick = (ms = 34) => { for (let i = 0; i < ms / 2.9; i++) e.pw_web_render(); t += ms; e.pw_web_control(t); };
const B = { OCTD:0,OCTU:1,ADD:2,PATCH:3,SCOPE:4,CABLES:5,EDIT:6,SHIFT:7,HOME:8,SAVE:9,PATCHES:10,SEQ:11,PLAY:12,REC:13 };
const press = (b) => { e.pw_web_set_buttons(1 << b); tick(); e.pw_web_set_buttons(0); tick(); };
const enc = (i, d) => { e.pw_web_encoder(i, d); tick(); };
function png(name) {
  const fb = new Uint16Array(e.memory.buffer, e.pw_web_fb(), 240 * 240);
  const raw = Buffer.alloc(240 * (240 * 3 + 1));
  for (let y = 0; y < 240; y++) { raw[y * 721] = 0; for (let x = 0; x < 240; x++) { const v = fb[y * 240 + x], o = y * 721 + 1 + x * 3;
    raw[o] = (v >> 11) << 3; raw[o + 1] = ((v >> 5) & 63) << 2; raw[o + 2] = (v & 31) << 3; } }
  const chunk = (type, data) => { const len = Buffer.alloc(4); len.writeUInt32BE(data.length); const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(zlib.crc32 ? zlib.crc32(td) : crc32(td)); return Buffer.concat([len, td, crc]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(240, 0); ihdr.writeUInt32BE(240, 4); ihdr[8] = 8; ihdr[9] = 2;
  fs.writeFileSync(new URL(`lcd/${name}.png`, import.meta.url), Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
}
function crc32(b){let c,crc=0xffffffff;for(let n=0;n<b.length;n++){c=(crc^b[n])&0xff;for(let k=0;k<8;k++)c=c&1?0xedb88320^(c>>>1):c>>>1;crc=(crc>>>8)^c;}return (crc^0xffffffff)>>>0;}
tick(); png('0_boot');
for (let i = 0; i < 60; i++) tick();
e.pw_web_set_keys(1 << 7); tick(200); png('1_rack_playing_key'); e.pw_web_set_keys(0); tick();
enc(0, 2); enc(2, 5); png('2_rack_sel_vcf');
press(B.EDIT); png('3_edit');
press(B.ADD); enc(0, 6); png('4_add');
press(B.SAVE); png('5_after_add_patch');
press(B.SAVE); tick(); png('6_connected');
press(B.CABLES); enc(0, 3); png('7_cables');
press(B.SCOPE); e.pw_web_set_keys(1<<7); for (let i=0;i<10;i++) tick(); png('8_scope'); e.pw_web_set_keys(0);
press(B.PATCHES); enc(0, 1); png('9_patches');
press(B.EDIT); press(B.PLAY); for (let i=0;i<30;i++) tick(); press(B.SEQ); png('10_seq');
press(B.HOME); for (let i=0;i<10;i++) tick(); png('11_rack_berlin');
e.pw_web_set_buttons(1<<B.SHIFT); tick(); png('12_shift'); e.pw_web_set_buttons(0); tick();
press(B.REC); png('13_delete_confirm');
console.log('ok', e.pw_web_patch_json(), new TextDecoder().decode(mem().slice(e.pw_web_buf(), e.pw_web_buf() + 300)));
