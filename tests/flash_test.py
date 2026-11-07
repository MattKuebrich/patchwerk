# The page's installer against a simulated FM-1 (device side of the M-UPGRADE protocol over a mock Web MIDI).
import asyncio, sys
from playwright.async_api import async_playwright
MOCK = r"""
(() => {
  const pack7 = (d) => { const o = []; let a = 0, n = 0; for (const b of d) { a |= b << n; n += 8; while (n >= 7) { o.push(a & 127); a >>>= 7; n -= 7; } } if (n) o.push(a & 127); return o; };
  const unpack7 = (s) => { const o = []; let a = 0, n = 0; for (const b of s) { a |= (b & 127) << n; n += 7; while (n >= 8) { o.push(a & 255); a >>>= 8; n -= 8; } } return o; };
  const dev = window.__dev = { mode: 'app', ident: 'FM-1_200', served: [], log: [], image: null };
  const listeners = []; const input = { name: 'FM-1', state: 'connected', onmidimessage: null, open: async () => {}, addEventListener: (t, f) => listeners.push(f) };
  const output = { name: 'FM-1', state: 'connected', open: async () => {}, send: (b) => setTimeout(() => recv(Array.from(b)), 2) };
  const tx = (bytes) => setTimeout(() => { dev.log.push('tx ' + bytes.length + (input.onmidimessage ? ' h' : ' NOHANDLER')); const ev = { data: new Uint8Array(bytes) }; input.onmidimessage && input.onmidimessage(ev); listeners.forEach((f) => f(ev)); }, 2);
  const identity = (txt) => { const d = [0, 0x59, 0x11, 27, 0, 0]; for (let i = 0; i < 27; i++) d.push(i < txt.length ? txt.charCodeAt(i) : 0); d.push(0); return [0xF0, ...pack7(d), 0xF7]; };
  const request = (addr, len, fl = 0) => { const u = [0, 0x59, 0x30, 8, 0, 0, fl, addr & 255, (addr >>> 8) & 255, (addr >>> 16) & 255, (addr >>> 24) & 255, len & 255, (len >> 8) & 255, 0]; let s = 0; for (let i = 6; i < 14; i++) s += u[i]; u.push(~s & 255); return [0xF0, ...pack7(u), 0xF7]; };
  let queue = [];
  const nextReq = () => { const r = queue.shift(); if (r) { dev.pending = r; dev.log.push('req ' + r[0].toString(16)); tx(request(r[0], r[1])); } };
  function recv(b) {
    if (dev.mode === 'gone') return;
    if (b.join() === [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7].join()) { tx(identity(dev.mode === 'ota' ? 'ota-FM1_1' : dev.ident)); return; }
    if (b.join() === [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7].join()) {
      dev.log.push('UPGRADE in ' + dev.mode);
      queue = dev.mode === 'app' ? [[0, 512], [4096, 256], [0xE0000000, 8]] : [[0, 512], [512, 512], [200000, 512], [0xF0000000, 8]];
      setTimeout(nextReq, 50); return;
    }
    const u = unpack7(b.slice(1, -1));
    if (u[2] === 0x30 && dev.pending) {          // a response to our read request
      const [addr, len] = dev.pending; dev.pending = null;
      const data = u.slice(14, 14 + (u[11] | (u[12] << 8)));
      dev.served.push([addr, data.length, data.slice(0, 4)]);
      if (addr === 0xE0000000) { dev.log.push('step1 done'); dev.mode = 'gone'; setTimeout(() => { dev.mode = 'ota'; }, 1500); return; }
      if (addr === 0xF0000000) { dev.log.push('step2 done'); dev.mode = 'gone'; setTimeout(() => { dev.mode = 'app'; dev.ident = dev.product; }, 1500); return; }
      setTimeout(nextReq, 5);
    }
  }
  navigator.requestMIDIAccess = async () => ({ inputs: new Map([['in', input]]), outputs: new Map([['out', output]]) });
})();
"""
async def main():
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path='/opt/pw-browsers/chromium-1194/chrome-linux/chrome')
        pg = await b.new_page(viewport={'width': 1400, 'height': 1000})
        errs = []
        pg.on('pageerror', lambda e: errs.append(str(e)))
        await pg.add_init_script(MOCK)
        await pg.goto('http://localhost:8766/index.html'); await pg.wait_for_timeout(800)
        # a file that is not a package
        await pg.set_input_files('#fw-file', files=[{'name': 'x.fwsc', 'mimeType': 'application/octet-stream', 'buffer': b'hello'}])
        await pg.wait_for_timeout(200); print('junk:', await pg.text_content('#fw-status'))
        # the stock file through the main picker: an FM-1 package, not PATCHWERK
        await pg.set_input_files('#fw-file', '/tmp/claude-0/FM-1.fwsc'); await pg.wait_for_timeout(300)
        print('pkg:', await pg.text_content('#fw-what'), '|', await pg.text_content('#fw-status'))
        await pg.evaluate("window.__dev.product = 'FM-1_015'")
        await pg.click('#fw-go')
        for _ in range(40):
            await pg.wait_for_timeout(500)
            st = await pg.text_content('#fw-status')
            if st.startswith('Done') or 'fail' in st.lower() or 'not ' in st: break
        print('page log:', (await pg.text_content('#fw-log'))[:600].replace('\n',' | '))
        print('install:', st, '| bar', await pg.evaluate("document.getElementById('fw-bar').value"))
        d = await pg.evaluate("window.__dev")
        print('device log:', d['log'], '| served', [(hex(a), n) for a, n, _ in d['served']])
        # the bytes served match the package's logical image
        ok = await pg.evaluate("""(async () => { const raw = new Uint8Array(await (await fetch('/FM-1.fwsc')).arrayBuffer());
            const img = FM1OTA.logicalImage(raw); return window.__dev.served.filter(s => s[0] < 0xE0000000).every(s => s[2].every((v, i) => v === img[s[0] + i])); })()""")
        print('served bytes match image:', ok)
        # the stock path checks the file's SHA-256
        await pg.click('summary:has-text("Back to M-VAVE")')
        await pg.set_input_files('#stock-file', '/tmp/claude-0/FM-1.fwsc'); await pg.wait_for_timeout(300)
        print('stock:', await pg.text_content('#fw-status'), '| button enabled:', await pg.is_enabled('#stock-go'))
        print('errors:', errs or 'none')
        await b.close()
asyncio.run(main())
