# The page linked to an FM-1: a simulated FM-1 behind a mock Web MIDI, made of the emulator's own engine with the
# FM-1's limits (web/build/patchwerk-fm1caps.wasm) for PATCHWERK's link, plus a stand-in for fm1-base's mirror (its
# screen sent as run-length coded rectangles, its panel played by the page's remote) and for the sample bank's
# upload (the FM-1 pulling the sound with read requests). Checks: the bay shows the FM-1's patch and edits it,
# the FM-1's edits show in the bay, the page's buttons press the FM-1's, the screen is the FM-1's, a sound goes
# into the FM-1's bank intact.
import asyncio, math, struct, sys, wave, io
from playwright.async_api import async_playwright
MOCK = r"""
(() => {
  const pack7 = (d) => { const o = []; let a = 0, n = 0; for (const b of d) { a |= b << n; n += 8; while (n >= 7) { o.push(a & 127); a >>>= 7; n -= 7; } } if (n) o.push(a & 127); return o; };
  const unpack7 = (s) => { const o = []; let a = 0, n = 0; for (const b of s) { a |= (b & 127) << n; n += 7; if (n >= 8) { o.push(a & 255); a >>>= 8; n -= 8; } } return o; };
  const frame = (cmd, body) => { let s = 0; for (const b of body) s += b; return [0xF0, ...pack7([0, 0x59, cmd, body.length & 255, (body.length >> 8) & 255, 0, ...body, ~s & 255]), 0xF7]; };
  const listeners = [];
  const input = { name: 'USB MIDI Device', state: 'connected', open: async () => {}, addEventListener: (t, f) => listeners.push(f), removeEventListener: () => {} };
  const tx = (bytes) => { const ev = { data: Uint8Array.from(bytes) }; listeners.forEach((f) => f(ev)); };
  const F = window.__fm1 = { dev: null, mirror: false, seq: 0, last: null, buttons: 0, keys: 0, remote: [], bank: {}, slots: {}, log: [], rx: 0 };
  const NS2PLAT = [12, 13, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11], PLAT2NS = []; NS2PLAT.forEach((p, i) => { PLAT2NS[p] = i; });
  const ENC_PLAT2NS = [0, 1, 6, 2, 3, 4, 5];
  const hash = (c) => (Math.imul(c, 40503) >>> 10) & 63;
  function rect(fb, y0) {                       /* one 240 x 16 band, mirror.c's code */
    const out = [0, y0, 240, 16, F.seq]; F.seq = (F.seq + 1) & 255;
    const cache = new Uint16Array(64); let cur = 0;
    for (let y = y0; y < y0 + 16; y++) {
      let i = 0;
      while (i < 240) {
        const v = fb[y * 240 + i]; let n = 1; while (i + n < 240 && fb[y * 240 + i + n] === v) n++; i += n;
        if (v !== cur) { const k = hash(v); if (cache[k] === v) { out.push(0x80 | k); n--; } else { const m = Math.min(n, 64); out.push(0xC0 | (m - 1), v >> 8, v & 255); cache[k] = v; n -= m; } cur = v; }
        while (n > 0) { const m = Math.min(n, 128); out.push(m - 1); n -= m; }
      }
    }
    return frame(0x51, out);
  }
  async function boot() {
    const bytes = await (await fetch('/patchwerk-fm1caps.wasm')).arrayBuffer();
    const D = (await WebAssembly.instantiate(bytes, {})).instance.exports;
    D.pw_web_flash_format(); D.pw_web_init(-1); D.pw_web_link_init(0); D.pw_web_set_knob(800);
    F.dev = D;
    let t = 0;
    setInterval(() => {
      t += 10;
      D.pw_web_set_buttons(F.buttons); D.pw_web_set_keys(F.keys);
      D.pw_web_control(t + 3000);
      for (let k = 0; k < 3; k++) D.pw_web_render();
      D.pw_web_link_poll(performance.now() | 0);
      for (let n; (n = D.pw_web_link_tx()) > 0;) tx(new Uint8Array(D.memory.buffer, D.pw_web_link_out(), n).slice());
      if (F.mirror) {
        const fb = new Uint16Array(D.memory.buffer, D.pw_web_fb(), 57600);
        for (let b = 0; b < 15; b++) {
          const band = fb.subarray(b * 3840, b * 3840 + 3840);
          if (F.last && F.last[b] && F.last[b].every((v, i) => v === band[i])) continue;
          (F.last = F.last || [])[b] = band.slice();
          tx(rect(fb, b * 16));
        }
        const leds = new Uint8Array(D.memory.buffer, D.pw_web_leds(), 41);
        let lit = 0, litk = 0;
        for (let i = 0; i < 14; i++) if (leds[i]) lit |= 1 << NS2PLAT[i];
        for (let k = 0; k < 27; k++) if (leds[14 + k]) litk |= 1 << k;
        let held = 0; for (let i = 0; i < 14; i++) if ((F.buttons >> i) & 1) held |= 1 << NS2PLAT[i];
        if (t % 100 === 0) tx(frame(0x53, [held & 255, held >> 8, ...[0, 8, 16, 24].map((s) => (F.keys >>> s) & 255), 0, 0, 0, 0, 0, 0, 0, 0, 16, lit & 255, lit >> 8, ...[0, 8, 16, 24].map((s) => (litk >>> s) & 255)]));
      }
    }, 10);
  }
  const ready = boot();
  const seaBytes = (f) => (Math.floor(f / 500) * 221 + (f % 500 ? 20 + Math.ceil((f % 500) / 40) + Math.ceil((f % 500) * 3 / 8) : 0) + 3) & ~3;   /* a SEA stream's size (core/pw_sea.inc) */
  async function bankPut(slot, frames, rate, crc, name, codec) {      /* the FM-1 pulling a sound, 512 bytes a request */
    const total = codec ? seaBytes(frames) : frames * 2, data = new Uint8Array(total);
    for (let at = 0; at < total; at += 512) {
      const len = Math.min(512, total - at);
      const got = await new Promise((res) => {
        F.want = { at, len, res };
        const u = [0, 0x59, 0x30, 8, 0, 0, 0, at & 255, (at >> 8) & 255, (at >> 16) & 255, (at >>> 24) & 255, len & 255, len >> 8, 0];
        let s = 0; for (let i = 6; i < 14; i++) s += u[i]; u.push(~s & 255);
        tx([0xF0, ...pack7(u), 0xF7]);
      });
      data.set(got, at);
    }
    let c = 0xFFFFFFFF; for (const b of data) { c ^= b; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; }
    const ok = ((c ^ 0xFFFFFFFF) >>> 0) === crc;
    if (ok) F.bank[slot] = { frames, rate, name, data, codec };
    F.log.push(`bank put B${slot + 1} ${frames} @${rate}${codec ? ' sea' : ''} crc ${ok ? 'ok' : 'BAD'}`);
    tx(frame(0x60, [1, ok ? 0 : 5]));
  }
  async function pull(total) {                                  /* the FM-1 pulling bytes from the page, 512 a request */
    const data = new Uint8Array(total);
    for (let at = 0; at < total; at += 512) {
      const len = Math.min(512, total - at);
      const got = await new Promise((res) => {
        F.want = { at, len, res };
        const u = [0, 0x59, 0x30, 8, 0, 0, 0, at & 255, (at >> 8) & 255, (at >> 16) & 255, (at >>> 24) & 255, len & 255, len >> 8, 0];
        let s = 0; for (let i = 6; i < 14; i++) s += u[i]; u.push(~s & 255);
        tx([0xF0, ...pack7(u), 0xF7]);
      });
      data.set(got, at);
    }
    return data;
  }
  const crcOf = (data) => { let c = 0xFFFFFFFF; for (const b of data) { c ^= b; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; } return (c ^ 0xFFFFFFFF) >>> 0; };
  function recv(b) {
    F.rx++;
    if (b.join() === [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7].join()) {   /* who are you */
      const d = [0, 0x59, 0x11, 27, 0, 0]; for (let i = 0; i < 27; i++) d.push(i < 10 ? 'FM-1_60600'.charCodeAt(i) : 0); d.push(0);
      tx([0xF0, ...pack7(d), 0xF7]); return;
    }
    if (b[1] === 0x7D) { const D = F.dev; new Uint8Array(D.memory.buffer, D.pw_web_buf(), b.length).set(b); D.pw_web_link_rx(b.length, performance.now() | 0); return; }
    const u = unpack7(b.slice(1, -1)), cmd = u[2], n = u[3] | (u[4] << 8), body = u.slice(6, 6 + n);
    if (cmd === 0x50) { F.mirror = body[0] !== 0; if (body[0] === 2) F.last = null; tx(frame(0x50, [3, F.mirror ? 1 : 0, 239, 239])); return; }
    if (cmd === 0x54) {
      const [seq, op, a, bb] = body;
      if (op === 1) { const i = PLAT2NS[a]; F.buttons = bb ? F.buttons | (1 << i) : F.buttons & ~(1 << i); }
      if (op === 2) F.keys = bb ? (F.keys | (1 << a)) >>> 0 : (F.keys & ~(1 << a)) >>> 0;
      if (op === 3) F.dev.pw_web_encoder(ENC_PLAT2NS[a], (bb << 24) >> 24);
      if (op === 0) { F.buttons = 0; F.keys = 0; }
      F.remote.push([op, a, bb]);
      tx(frame(0x54, [seq, op, 1])); return;
    }
    if (cmd === 0x30 && F.want) {             /* the page's answer to a read request */
      const addr = u[7] | (u[8] << 8) | (u[9] << 16) | (u[10] << 24), len = u[11] | (u[12] << 8);
      if (addr === F.want.at && len === F.want.len) { const w = F.want; F.want = null; w.res(u.slice(14, 14 + len)); }
      return;
    }
    if (cmd === 0x60) {
      if (body[0] === 1) { const r32 = (o) => (body[o] | (body[o + 1] << 8) | (body[o + 2] << 16) | (body[o + 3] << 24)) >>> 0;
        setTimeout(() => bankPut(body[1], r32(2), r32(6), r32(10), String.fromCharCode(...body.slice(14, 26)).replace(/\0+$/, ''), body.length > 26 ? body[26] : 0), 20); return; }
      const r32 = (o) => (body[o] | (body[o + 1] << 8) | (body[o + 2] << 16) | (body[o + 3] << 24)) >>> 0;
      if (body[0] === 5) {                    /* a user patch, a chunk at a time */
        const d = F.slots[body[1]], off = body[2] | (body[3] << 8);
        if (!d) { tx(frame(0x60, [5, 1, body[1]])); return; }
        const k = Math.min(248, d.length - off);
        tx(frame(0x60, [5, 0, body[1], d.length & 255, d.length >> 8, off & 255, off >> 8, k, ...d.slice(off, off + k)])); return;
      }
      if (body[0] === 6) {                    /* a user patch pulled from the page */
        const slot = body[1], total = body[2] | (body[3] << 8), crc = r32(4);
        setTimeout(async () => { const d = await pull(total); const ok = crcOf(d) === crc; if (ok) F.slots[slot] = d; F.log.push(`slot put U${slot + 1} ${ok ? 'ok' : 'BAD'}`); tx(frame(0x60, [6, ok ? 0 : 5])); }, 20);
        return;
      }
      if (body[0] === 8) {                    /* a sound back, a chunk at a time */
        const e = F.bank[body[1]], off = r32(2);
        if (!e) { tx(frame(0x60, [8, 1, body[1]])); return; }
        const k = Math.min(248, e.data.length - off);
        tx(frame(0x60, [8, 0, body[1], ...[0, 8, 16, 24].map((s) => (off >>> s) & 255), k, ...e.data.slice(off, off + k)])); return;
      }
      if (body[0] === 4) {                    /* the listing: 24 slots an answer ([4]: 0-23; [4, 24]: A2's 24-39) */
        const first = body.length > 1 ? body[1] : 0, cnt = Math.max(0, Math.min(24, 40 - first));
        const out = [4, 0, cnt, first, ...[0, 8, 16, 24].map((s) => (200000 >>> s) & 255)];
        for (let i = first; i < first + cnt; i++) { const e = F.bank[i]; const f = e ? e.frames : 0, r = e ? e.rate : 0;
          out.push(...[0, 8, 16, 24].map((s) => (f >>> s) & 255), ...[0, 8, 16, 24].map((s) => ((r | (e && e.codec ? 0x80000000 : 0)) >>> s) & 255), ...[...(e ? e.name : '').padEnd(12, '\0')].map((c) => c.charCodeAt(0))); }
        out.push(...[0, 8, 16, 24].map((s) => (0x4F000 >>> s) & 255), ...[0, 8, 16, 24].map((s) => (0x3F000 >>> s) & 255));   /* rc54: the room in all the windows, and what is free of it */
        tx(frame(0x60, out)); return;
      }
      tx(frame(0x60, [body[0], 0])); return;
    }
  }
  const output = { name: 'USB MIDI Device', state: 'connected', open: async () => {}, send: (b) => { const a = Array.from(b); ready.then(() => setTimeout(() => recv(a), 1)); } };
  const other = { name: 'IAC Bus 1', state: 'connected', open: async () => {}, addEventListener: () => {}, removeEventListener: () => {}, send: () => {} };
  navigator.requestMIDIAccess = async () => { await ready; return { inputs: new Map([['x', other], ['in', input]]), outputs: new Map([['y', other], ['out', output]]), onstatechange: null }; };
})();
"""
def wav_bytes():
    b = io.BytesIO(); w = wave.open(b, 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100)
    w.writeframes(struct.pack('<%dh' % 13230, *[int(12000 * math.sin(2 * math.pi * 220 * i / 44100)) for i in range(13230)])); w.close()
    return b.getvalue()
async def main():
    fails = []
    def check(ok, what):
        print(('  ok   ' if ok else 'FAIL   ') + what)
        if not ok: fails.append(what)
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path='/opt/pw-browsers/chromium-1194/chrome-linux/chrome')
        pg = await b.new_page(viewport={'width': 1400, 'height': 1100})
        errs = []
        pg.on('pageerror', lambda e: errs.append(str(e)))
        await pg.add_init_script(MOCK)
        await pg.goto('http://localhost:8766/index.html'); await pg.wait_for_timeout(1000)
        dev = lambda js: pg.evaluate(f"(() => {{ const D = window.__fm1.dev; return {js}; }})()")
        bay = lambda: pg.evaluate("document.querySelectorAll('.mod').length")
        await pg.evaluate("window.__fm1.dev.pw_web_factory(1)")           # the FM-1 has BERLIN SEQ; the page BASIC VOICE
        await pg.click('#link-btn'); await pg.wait_for_timeout(2500)
        check(await bay() == 9, f"the bay shows the FM-1's patch (9 modules: {await bay()})")
        st = await pg.text_content('#link-status'); check(st.startswith('Linked'), f'status: {st}')
        check(await pg.is_hidden('#link-banner'), 'the "not linked" banner is gone once linked')
        # the FM-1's edit shows in the bay
        await pg.evaluate("window.__fm1.dev.pw_web_remove(3)"); await pg.wait_for_timeout(600)
        check(await bay() == 8, f'a module removed on the FM-1 leaves the bay ({await bay()})')
        # the bay's edit happens on the FM-1
        await pg.locator('.mod').first.locator('header button.rm').click(); await pg.wait_for_timeout(600)
        n = await dev("JSON.parse(new TextDecoder().decode(new Uint8Array(D.memory.buffer, D.pw_web_buf(), D.pw_web_patch_json()))).modules.length")
        check(n == 7, f'a module removed in the bay leaves the FM-1 ({n})')
        await pg.select_option('#factory', '0'); await pg.wait_for_timeout(1500)
        n = await dev("JSON.parse(new TextDecoder().decode(new Uint8Array(D.memory.buffer, D.pw_web_buf(), D.pw_web_patch_json()))).name")
        check(n == 'BASIC VOICE', f'a factory patch loaded in the bay is loaded on the FM-1 ({n})')
        # the page's buttons press the FM-1's: ARP opens its performance page
        await pg.locator('.fbtn[aria-label^="ARP "]').click(); await pg.wait_for_timeout(500)
        check(await dev("D.pw_web_ui_page()") == 4, f"ARP on the page: the FM-1's matrix page (page {await dev('D.pw_web_ui_page()')})")
        await pg.locator('.dial').nth(1).dispatch_event('wheel', {'deltaY': -100}); await pg.wait_for_timeout(400)
        await pg.locator('.fbtn[aria-label^="HOME "]').click(); await pg.wait_for_timeout(600)
        check(await dev("D.pw_web_ui_page()") == 1, 'HOME on the page: the patch')
        # the screen is the FM-1's
        same = await pg.evaluate("""(() => { const D = window.__fm1.dev, fb = new Uint16Array(D.memory.buffer, D.pw_web_fb(), 57600);
            const c = document.getElementById('lcd').getContext('2d').getImageData(0, 0, 240, 240).data; let bad = 0;
            for (let i = 0; i < 57600; i += 97) { const v = fb[i]; if (Math.abs(c[i * 4] - ((v >> 11) & 31) * 255 / 31) > 2 || Math.abs(c[i * 4 + 1] - ((v >> 5) & 63) * 255 / 63) > 2) bad++; }
            return bad; })()""")
        check(same == 0, f"the page's screen shows the FM-1's ({same} pixels differ of 594)")
        lit = await pg.evaluate("document.querySelector('.fbtn[aria-label^=\"HOME \"]').classList.contains('lit')")
        check(lit, "the FM-1's lit HOME button lights on the page")
        await pg.locator('#lcd').screenshot(path='lcd/linked.png')
        # a knob in the bay: the FM-1's parameter follows
        await pg.evaluate("window.__fm1.dev.pw_web_set_param(1, 0, 0)"); await pg.wait_for_timeout(400)
        k = pg.locator('.mod').nth(1).locator('.kn svg').first
        for _ in range(5): await k.dispatch_event('wheel', {'deltaY': -100, 'ctrlKey': True}); await pg.wait_for_timeout(30)
        await pg.wait_for_timeout(500)
        v = await dev("JSON.parse(new TextDecoder().decode(new Uint8Array(D.memory.buffer, D.pw_web_buf(), D.pw_web_patch_json()))).modules[1].p[0]")
        check(v > 1, f"a knob turned in the bay turns the FM-1's (VCO tune {v:.2f})")
        # a sound into the FM-1's bank
        await pg.set_input_files('#bank .bslot:nth-child(2) input[type=file]', files=[{'name': 'Low Sine.wav', 'mimeType': 'audio/wav', 'buffer': wav_bytes()}])
        await pg.wait_for_timeout(800)
        await pg.click('#bank .bslot:nth-child(2) [data-a=send]')
        for _ in range(60):
            await pg.wait_for_timeout(250)
            log = await pg.evaluate("window.__fm1.log")
            if log: break
        await pg.wait_for_timeout(600)
        check(log == ['bank put B2 13240 @44100 sea crc ok'], f'the sound arrives in the FM-1 compressed and intact ({log})')
        dv = await pg.get_attribute('#bank .bslot:nth-child(2) .devtxt', 'title')
        check((dv or '').startswith('On the FM-1: LOW SINE') and '(compressed)' in dv, f'the page lists it on the FM-1, compressed ({dv})')
        sent = await pg.evaluate("[...window.__fm1.bank[1].data]")
        mine = await pg.evaluate("[...window.__patchwerkBank.BANK.slots[1].z.bytes]")
        check(sent == mine and len(sent) < 13230 * 2 // 3, f'the FM-1 holds the stream the page made ({len(sent)} bytes for 13230 frames of 16-bit)')
        # Compress off: the sound goes as it is, 16-bit
        await pg.evaluate("window.__fm1.log = []")
        await pg.uncheck('#bank .bslot:nth-child(2) [data-a=comp]'); await pg.wait_for_timeout(300)
        await pg.click('#bank .bslot:nth-child(2) [data-a=send]')
        for _ in range(60):
            await pg.wait_for_timeout(250)
            log = await pg.evaluate("window.__fm1.log")
            if log: break
        await pg.wait_for_timeout(600)
        check(log == ['bank put B2 13230 @44100 crc ok'], f'Compress off: it arrives as it is ({log})')
        await pg.check('#bank .bslot:nth-child(2) [data-a=comp]'); await pg.wait_for_timeout(300)
        await pg.evaluate("window.__fm1.log = []")
        await pg.click('#bank .bslot:nth-child(2) [data-a=send]')
        for _ in range(60):
            await pg.wait_for_timeout(250)
            log = await pg.evaluate("window.__fm1.log")
            if log: break
        await pg.wait_for_timeout(600)
        await pg.wait_for_timeout(3000)
        st = await pg.text_content('#link-status'); check(st.startswith('Linked'), f'linked again after the upload: {st}')
        # a second sound (A1-1), then one to A2: the earlier ones stay "on the FM-1", the sending text goes, the storage shows
        async def send_to(sel, name, frames):
            await pg.evaluate("window.__fm1.log = []")
            await pg.set_input_files(sel + ' input[type=file]', files=[{'name': name, 'mimeType': 'audio/wav', 'buffer': wav_bytes()}])
            await pg.wait_for_timeout(800)
            await pg.click(sel + ' [data-a=send]')
            for _ in range(80):
                await pg.wait_for_timeout(250)
                if await pg.evaluate("window.__fm1.log.length"): break
            await pg.wait_for_timeout(2500)
        await send_to('#bank .bslot:nth-child(1)', 'Kick One.wav', 8000)
        d2 = await pg.text_content('#bank .bslot:nth-child(2) .devtxt')
        d1 = await pg.text_content('#bank .bslot:nth-child(1) .devtxt')
        check('on the fm-1' in d2.lower() and 'on the fm-1' in d1.lower(), f'after a second send the first is still on the FM-1 ({d1!r}, {d2!r})')
        check('Sending' not in d1 + d2, f'the sending text is gone when it is done ({d1!r})')
        await send_to('#bank3 .bslot:nth-child(1)', 'Snare C.wav', 6000)
        dA = await pg.text_content('#bank3 .bslot:nth-child(1) .devtxt')
        check('on the fm-1' in dA.lower(), f'a sound in C (slot 24) lands on the FM-1 ({dA!r})')
        check(await pg.evaluate("window.__fm1.bank[24] && window.__fm1.bank[24].data.length > 0"), 'the FM-1 holds it in bank slot 24')
        szt = await pg.text_content('#bank3 .bslot:nth-child(1) .sz')
        check('KB' in szt or ' B' in szt, f'the size is shown ({szt!r})')
        free = await pg.text_content('#bank-free')
        check('used of' in free and '316 KB' in free, f'the FM-1 storage line: {free}')
        check('remaining' in free, f'the storage line says what is left: {free}')
        # no room: the pad says so (the page believes the FM-1 has one free sector)
        await pg.evaluate("window.__patchwerkBank.BANK.free = 4096; window.__fm1.log = []")
        await pg.set_input_files('#bank .bslot:nth-child(5) input[type=file]', files=[{'name': 'Big.wav', 'mimeType': 'audio/wav', 'buffer': wav_bytes()}])
        await pg.wait_for_timeout(800)
        await pg.click('#bank .bslot:nth-child(5) [data-a=send]'); await pg.wait_for_timeout(600)
        nr = await pg.text_content('#bank .bslot:nth-child(5) .devtxt')
        check(nr.startswith('No room') and await pg.evaluate("window.__fm1.log.length") == 0, f'a sound that does not fit says so on its pad ({nr!r})')
        h = await pg.evaluate("[...document.querySelectorAll('#bank .bslot')].map((e) => Math.round(e.getBoundingClientRect().height))")
        check(len(set(h)) == 1, f'every pad is the same height ({sorted(set(h))})')
        await pg.screenshot(path='/tmp/shot_linked.png')
        # a backup from the FM-1: its user patch U3 and its sound B2, in a file
        await pg.evaluate("(() => { const D = window.__fm1.dev, n = D.pw_web_save(); window.__fm1.slots[2] = new Uint8Array(D.memory.buffer, D.pw_web_buf(), n).slice(); })()")
        await pg.check('#bk-from-fm1')
        try:
            async with pg.expect_download(timeout=30000) as dl:
                await pg.click('#bk-go')
        except Exception:
            print('status:', await pg.text_content('#bk-status'), await pg.evaluate("window.__fm1.log"))
            raise
        path = await (await dl.value).path()
        import json
        bk = json.load(open(path))
        check(bk['from'] == 'FM-1' and bk['slots'][2] and not bk['slots'][0] and bk['bank'][1] and bk['bank'][1]['name'] == 'LOW SINE' and bk['bank'][1].get('z') and bk['bank'][1]['zf'] == 13240,
              f"the backup file has U3 and B2 ({[i for i, x in enumerate(bk['slots']) if x]}, {[b and b['name'] for b in bk['bank']]})")
        st = await pg.text_content('#bk-status'); check('1 user patch' in st and '3 sounds' in st, f'backup status: {st}')
        # restored into another FM-1 slot: the file's U3 written back (the mock's slot emptied first)
        await pg.evaluate("delete window.__fm1.slots[2]; window.__fm1.log = []")
        await pg.set_input_files('#bk-file', path)
        await pg.wait_for_timeout(300)
        await pg.check('#bk-to-fm1'); await pg.uncheck('#bk-r-cur')
        await pg.click('#bk-restore-go'); await pg.click('#bk-restore-go')
        for _ in range(40):
            await pg.wait_for_timeout(250)
            if 'Restored' in (await pg.text_content('#bk-r-status')) or 'not' in (await pg.text_content('#bk-r-status')): break
        log = await pg.evaluate("window.__fm1.log"); st = await pg.text_content('#bk-r-status')
        same = await pg.evaluate("(() => { const a = window.__fm1.slots[2], D = window.__fm1.dev; return !!a && a.length > 28; })()")
        check(log == ['slot put U3 ok'] and same and 'Restored 1' in st, f'restored to the FM-1: {log}, {st}')
        # the sound alone, from the backup, back to the FM-1: the same stream, not packed a second time
        await pg.evaluate("window.__fm1.log = []; window.__fm1.bank = {}")
        await pg.set_input_files('#bk-file', path); await pg.wait_for_timeout(300)
        await pg.check('#bk-to-fm1'); await pg.uncheck('#bk-r-cur'); await pg.uncheck('#bk-r-slots'); await pg.check('#bk-r-bank')
        await pg.click('#bk-restore-go'); await pg.click('#bk-restore-go')
        for _ in range(60):
            await pg.wait_for_timeout(250)
            if 'Restored' in (await pg.text_content('#bk-r-status')) or 'not' in (await pg.text_content('#bk-r-status')): break
        log = await pg.evaluate("window.__fm1.log"); again = await pg.evaluate("window.__fm1.bank[1] ? [...window.__fm1.bank[1].data] : null")
        check(sorted(log) == ['bank put B1 13240 @44100 sea crc ok', 'bank put B2 13240 @44100 sea crc ok', 'bank put B25 13240 @44100 sea crc ok'] and again == sent, f'the backup\'s sound goes back to the FM-1 as the same stream ({log})')
        await pg.wait_for_timeout(2500)
        # unlink: the emulator keeps the patch
        await pg.click('#link-btn'); await pg.wait_for_timeout(500)
        check(await bay() == 6, f"unlinked, the bay keeps the FM-1's patch ({await bay()})")
        check(not errs, f'no page errors {errs}')
        await b.close()
    print('LINK PAGE TESTS PASSED' if not fails else f'{len(fails)} LINK PAGE FAILURES')
    sys.exit(1 if fails else 0)
asyncio.run(main())
