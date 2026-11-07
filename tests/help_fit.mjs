// Every module's description fits the add list on the FM-1's screen (8 lines at most; 7 leaves its jacks room).
import fs from 'fs';
const { instance } = await WebAssembly.instantiate(fs.readFileSync(new URL('../web/build/patchwerk.wasm', import.meta.url)), {});
const e = instance.exports; e.pw_web_flash_format(); e.pw_web_init(-1);
const types = JSON.parse(new TextDecoder().decode(new Uint8Array(e.memory.buffer, e.pw_web_buf(), e.pw_web_types_json()))).types;
const over = types.map((t, i) => [t.name, e.pw_web_help_lines(i)]).filter(([, n]) => n > 8);
const most = Math.max(...types.map((t, i) => e.pw_web_help_lines(i)));
if (over.length) { console.log('DESCRIPTIONS TOO LONG:', over.map(([n, k]) => `${n} (${k} lines)`).join(', ')); process.exit(1); }
const ex = types.map((t, i) => [t.name, e.pw_web_example_lines(i)]);
const exOver = ex.filter(([, n]) => n > 7);
if (exOver.length) { console.log('EXAMPLES TOO LONG (7 lines fit the add list):', exOver.map(([n, k]) => `${n} (${k})`).join(', ')); process.exit(1); }
const xs = types.map((t, i) => [t.name, e.pw_web_help_lines_xs(i), e.pw_web_example_lines_xs(i)]);
const xsBad = xs.filter(([, h, x]) => h > 8 || x > 7);
if (xsBad.length) { console.log('TOO LONG UNDER THE PATCH STRIP (8 description lines, 7 example lines):', xsBad.map(([n, h, x]) => `${n} (${h}, ${x})`).join(', ')); process.exit(1); }
console.log(`EXAMPLES FIT (the longest ${Math.max(...ex.map(([, n]) => n))} lines of 7)`);
console.log(`DESCRIPTIONS FIT (the longest ${most} lines of 8)`);
