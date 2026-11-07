# The page's Backup section and sample grid, with the emulator (no FM-1): user patches and the bank into a file,
# back from it; one patch as a file; several sounds dropped at once. Needs the page served on :8766.
import asyncio, io, json, math, struct, wave
from playwright.async_api import async_playwright
URL = 'http://localhost:8766/index.html'
def wav_bytes(hz):
    b = io.BytesIO(); w = wave.open(b, 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100)
    w.writeframes(struct.pack('<%dh' % 8820, *[int(12000 * math.sin(2 * math.pi * hz * i / 44100)) for i in range(8820)])); w.close()
    return list(b.getvalue())
async def main():
    fails = []
    def check(ok, what):
        print(('  ok   ' if ok else '  FAIL ') + what)
        if not ok: fails.append(what)
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path='/opt/pw-browsers/chromium-1194/chrome-linux/chrome')
        pg = await b.new_page(viewport={'width': 1400, 'height': 1100})
        errs = []
        pg.on('pageerror', lambda e: errs.append(str(e)))
        await pg.goto(URL); await pg.wait_for_timeout(1500)
        # the first visit: both banks empty (the factory sounds are INTERNAL, built in)
        names16 = await pg.evaluate("[...document.querySelectorAll('#bank .bslot header span')].map((e) => e.textContent)")
        names32 = await pg.evaluate("[...document.querySelectorAll('#bank2 .bslot header span')].map((e) => e.textContent)")
        check(names16.count('empty') == 8 and names32.count('empty') == 8, f'first visit: A and B empty ({names16[0]}, {names32[0]})')
        cols = await pg.evaluate("[document.getElementById('bankcol1').getBoundingClientRect().top, document.getElementById('bankcol2').getBoundingClientRect().top]")
        hh = await pg.evaluate("[...document.querySelectorAll('.banks .bslot')].map((e) => Math.round(e.getBoundingClientRect().height))")
        check(len(set(hh)) == 1, f'empty and filled pads are the same height ({sorted(set(hh))})')
        bb = await pg.evaluate("[...document.querySelectorAll('.bank-bar .tbtn')].map((e) => Math.round(e.getBoundingClientRect().top))")
        check(len(set(bb)) == 1, f'the buttons are on one line ({sorted(set(bb))})')
        check(abs(cols[0] - cols[1]) < 2, f'A and B side by side ({cols})')
        # an old visit's seeded factory sounds in A1 are cleared once
        await pg.click('#bank-factory'); await pg.click('#bank-factory'); await pg.wait_for_timeout(300)
        names16 = await pg.evaluate("[...document.querySelectorAll('#bank .bslot header span')].map((e) => e.textContent)")
        names8b = await pg.evaluate("[...document.querySelectorAll('#bank2 .bslot header span')].map((e) => e.textContent)")
        check(names16[0] == '808 KICK' and names16[7] == '808 RIM' and names8b[0] == '909 KICK' and names8b[7] == '909 CYMBAL', f'808 + 909 to A+B fills A and B ({names16[0]}, {names16[7]}, {names8b[0]}, {names8b[7]})')
        await pg.evaluate("localStorage.removeItem('patchwerk-bank-internal')")
        await pg.reload(); await pg.wait_for_timeout(1500)
        names16 = await pg.evaluate("[...document.querySelectorAll('#bank .bslot header span')].map((e) => e.textContent)")
        check(names16.count('empty') == 8, 'a bank that is only the old factory seed is cleared once')
        async def press(name):
            loc = pg.locator(f'.fbtn[aria-label^="{name} "]'); await loc.scroll_into_view_if_needed(); bb = await loc.bounding_box()
            await pg.mouse.move(bb['x'] + bb['width'] / 2, bb['y'] + bb['height'] * 0.6); await pg.mouse.down(); await pg.wait_for_timeout(200); await pg.mouse.up(); await pg.wait_for_timeout(450)
        async def factory(i):
            await pg.select_option('#factory', str(i)); await pg.wait_for_timeout(500)
        async def download(btn):
            async with pg.expect_download(timeout=30000) as dl:
                await pg.click(btn)
            return json.load(open(await (await dl.value).path()))
        # U1: FM BELLS (SAVE, SAVE)
        await factory(6); await press('SAVE'); await press('SAVE'); await press('SAVE'); await pg.wait_for_timeout(600)
        # three sounds dropped on the sample section at once: the empty slots fill in turn
        await pg.evaluate("""(files) => { const dt = new DataTransfer();
            files.forEach(([n, b]) => dt.items.add(new File([new Uint8Array(b)], n, { type: 'audio/wav' })));
            document.getElementById('samples').dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true })); }""",
            [['one.wav', wav_bytes(220)], ['two.wav', wav_bytes(330)], ['three.wav', wav_bytes(440)]])
        await pg.wait_for_timeout(1500)
        names = await pg.evaluate("[...document.querySelectorAll('#bank .bslot header span')].slice(0, 4).map((e) => e.textContent)")
        check(names == ['ONE', 'THREE', 'TWO', 'empty'], f'three sounds dropped: A-1..3, in name order ({names})')
        cols = await pg.evaluate("getComputedStyle(document.getElementById('bank')).gridTemplateColumns.split(' ').length")
        check(cols == 4, f'the bank a 4 x 4 grid ({cols} columns)')
        # the backup
        bk = await download('#bk-go')
        check(bk['patchwerk'] == 'backup' and bk['slots'][0] and not bk['slots'][1] and bk['current'] and len(bk['bank']) == 40 and bk['bank'][1]['name'] == 'THREE' and bk['bank'][1].get('tr') == 1 and bk['bank'][1].get('cp') == 1,
              f"backup: U1, the patch playing, 3 sounds ({[i for i, x in enumerate(bk['slots']) if x]}, {[s and s['name'] for s in bk['bank'][:4]]})")
        # ACID 303 over U1 (SAVE stores where the patch came from), a sound cleared; then the file restored
        await factory(12); await press('HOME')
        for k in range(3): await press('SAVE'); await pg.wait_for_timeout(400)
        await pg.wait_for_timeout(600)
        await pg.click('#bank .bslot:nth-child(2) [data-a=clear]'); await pg.wait_for_timeout(200)
        b2 = await download('#bk-go')
        check(b2['slots'][0] != bk['slots'][0] and not b2['bank'][1], f"U1 overwritten (ACID 303) and A-2 cleared before the restore ({b2['slots'][0] != bk['slots'][0]}, {[x and x['name'] for x in b2['bank'][:3]]})")
        path = '/tmp/patchwerk_backup_test.json'; json.dump(bk, open(path, 'w'))
        await pg.set_input_files('#bk-file', path); await pg.wait_for_timeout(300)
        listed = await pg.evaluate("[...document.querySelectorAll('#bk-list li')].map((l) => l.textContent)")
        check(any('U1' in x and 'FM BELLS' in x for x in listed), f'the file listed: {listed[:3]}')
        await pg.check('#bk-r-bank')
        await pg.click('#bk-restore-go'); await pg.click('#bk-restore-go'); await pg.wait_for_timeout(1500)
        st = await pg.text_content('#bk-r-status')
        b3 = await download('#bk-go')
        check(b3['slots'][0] == bk['slots'][0] and b3['bank'][1] and b3['bank'][1]['name'] == 'THREE', f'restored: U1 as it was, A-2 back ({st})')
        name = await pg.evaluate("window.__patchwerk.patch().name")
        check(name == 'FM BELLS', f'the patch that was playing loaded ({name})')
        # one patch as a file
        await factory(9)
        pf = await download('#pf-save')
        check(pf['patchwerk'] == 'patch' and pf['name'] == 'GLITCH LOOP', f"a patch file ({pf.get('name')})")
        await factory(1)
        json.dump(pf, open('/tmp/patchwerk_patch_test.json', 'w'))
        await pg.set_input_files('#pf-file', '/tmp/patchwerk_patch_test.json'); await pg.wait_for_timeout(600)
        name = await pg.evaluate("window.__patchwerk.patch().name")
        check(name == 'GLITCH LOOP', f'the patch file opened ({name})')
        await pg.evaluate("document.getElementById('backup').scrollIntoView()"); await pg.wait_for_timeout(200)
        await pg.screenshot(path='tests/out/backup.png')
        await pg.evaluate("document.getElementById('samples').scrollIntoView()"); await pg.wait_for_timeout(200)
        await pg.screenshot(path='tests/out/samples.png')
        # the two boxes: Trim and Compress, each its own
        await pg.check('#bank .bslot:nth-child(1) [data-a=trim]'); await pg.wait_for_timeout(200)
        s1 = await pg.evaluate("(() => { const s = window.__patchwerkBank.BANK.slots[0]; return [s.trim, s.comp]; })()")
        await pg.uncheck('#bank .bslot:nth-child(1) [data-a=comp]'); await pg.wait_for_timeout(200)
        s2 = await pg.evaluate("(() => { const s = window.__patchwerkBank.BANK.slots[0]; return [s.trim, s.comp]; })()")
        sz = await pg.text_content('#bank .bslot:nth-child(1) .sz')
        check(s1 == [True, True] and s2 == [True, False] and 'packed' not in sz and 'KB' in sz, f'Trim and Compress are separate boxes ({s1}, {s2}, size "{sz}")')
        # several files dropped on a bank (not a slot): from sound 1, in name order, up to 16
        await pg.evaluate("""(files) => { const dt = new DataTransfer();
            files.forEach(([n, b]) => dt.items.add(new File([new Uint8Array(b)], n, { type: 'audio/wav' })));
            document.getElementById('bankcol2').dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true })); }""",
            [['snare 10.wav', wav_bytes(300)], ['snare 2.wav', wav_bytes(310)], ['kick.wav', wav_bytes(100)]])
        await pg.wait_for_timeout(1500)
        names = await pg.evaluate("[...document.querySelectorAll('#bank2 .bslot header span')].slice(0, 4).map((e) => e.textContent)")
        check(names == ['KICK', 'SNARE 2', 'SNARE 10', 'empty'], f'three files dropped on B fill it from sound 1, in name order ({names})')
        a1 = await pg.evaluate("[...document.querySelectorAll('#bank .bslot header span')].slice(0, 3).map((e) => e.textContent)")
        check(a1 == ['ONE', 'THREE', 'TWO'], f'...and A is untouched ({a1})')
        await pg.click('#bank-factory2'); await pg.click('#bank-factory2'); await pg.wait_for_timeout(400)
        nc = await pg.evaluate("[...document.querySelectorAll('#bank3 .bslot header span')].map((e) => e.textContent)")
        nd = await pg.evaluate("[...document.querySelectorAll('#bank4 .bslot header span')].map((e) => e.textContent)")
        check(nc[0] == 'FM KICK' and nc[7] == 'FM CRASH' and nd[0] == 'BOOM BRK' and nd[7] == 'VOICE OH', f'FM + Random to C+D ({nc[0]}, {nc[7]}, {nd[0]}, {nd[7]})')
        # Clear bank: one bank's eight slots, after a second click
        await pg.click('[data-clearbank="1"]'); await pg.wait_for_timeout(200)
        mid = await pg.evaluate("[...document.querySelectorAll('#bank2 .bslot header span')].filter((e) => e.textContent !== 'empty').length")
        await pg.click('[data-clearbank="1"]'); await pg.wait_for_timeout(300)
        after = await pg.evaluate("[[...document.querySelectorAll('#bank2 .bslot header span')].filter((e) => e.textContent !== 'empty').length, [...document.querySelectorAll('#bank3 .bslot header span')].filter((e) => e.textContent !== 'empty').length]")
        check(mid > 0 and after == [0, 8], f'Clear bank asks first, then clears just that bank (before {mid}, after {after})')
        free = await pg.text_content('#bank-free')
        check('of the FM-1' in free and 'KB' in free and ('remaining' in free or 'over' in free), f'the storage line before linking: {free}')
        await pg.evaluate("document.getElementById('modules').scrollIntoView()"); await pg.wait_for_timeout(200)
        await pg.screenshot(path='tests/out/modules.png')
        h = await pg.evaluate("document.getElementById('modules').offsetHeight")
        check(h < 3000, f'the module section is compact ({h} px)')
        check(not errs, f'no page errors {errs}')
        await b.close()
    print('BACKUP TESTS PASSED' if not fails else f'{len(fails)} BACKUP TESTS FAILED')
asyncio.run(main())
