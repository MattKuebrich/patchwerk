// The installer's handshake against M-UPGRADE 3.11's short identities: a PATCHWERK answers the plain query with
// FM-1_606 (a name and three digits) and the whole identity (FM-1_606010000) to the installers' query (body 01).
import { Updater, pack7, unpack7 } from '../base/web/fm1ota.js';
let bad = 0;
const check = (what, ok) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) bad++; };
const identity = (txt) => { const d = [0, 0x59, 0x11, 27, 0, 0]; for (let i = 0; i < 27; i++) d.push(i < txt.length ? txt.charCodeAt(i) : 0); d.push(0); return [0xF0, ...pack7(d), 0xF7]; };
function fake(plainId, fullId) {
  const dev = { plain: 0, full: 0 };
  const input = { name: 'FM-1', state: 'connected', onmidimessage: null, open: async () => {} };
  const output = { name: 'FM-1', state: 'connected', open: async () => {}, send: (b) => {
    const bytes = Array.from(b), u = unpack7(bytes.slice(1, -1));
    let reply = null;
    if (bytes.join() === [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7].join()) { dev.plain++; reply = identity(plainId); }
    else if (u[2] === 0x11 && u[3] === 1 && u[6] === 1) { dev.full++; if (fullId) reply = identity(fullId); }
    if (reply) setTimeout(() => input.onmidimessage && input.onmidimessage({ data: new Uint8Array(reply) }), 1);
  } };
  dev.access = { inputs: new Map([['i', input]]), outputs: new Map([['o', output]]) };
  return dev;
}
let d = fake('FM-1_606', 'FM-1_606010000');
let r = await new Updater(d.access).find();
check('a PATCHWERK: the whole identity from the installers\' query', r && r.id.text === 'FM-1_606010000' && r.id.model === 'FM-1' && d.full === 1);
d = fake('FM-1_606010000', null);
r = await new Updater(d.access).find();
check('an older PATCHWERK (the long identity at once): taken as it is, no second query', r && r.id.text === 'FM-1_606010000' && d.full === 0);
d = fake('FM-1_606', null);
r = await new Updater(d.access).find();
check('no answer to the second query: the short identity stands', r && r.id.text === 'FM-1_606' && d.full === 1);
d = fake('FM-1_015', 'FM-1_015999');
r = await new Updater(d.access).find();
check('M-VAVE\'s stock FM-1 (FM-1_015): never asked again', r && r.id.text === 'FM-1_015' && d.full === 0);
d = fake('ota-FM-1_015', null);
r = await new Updater(d.access).find();
check('the update loader (ota-FM-1_015): never asked again', r && r.id.text === 'ota-FM-1_015' && d.full === 0);
console.log(bad ? 'IDENT FAILED' : 'IDENT PASSED');
process.exit(bad ? 1 : 0);
