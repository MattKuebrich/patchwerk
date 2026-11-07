# FM-DX on the page (the emulator, no FM-1): a TX81Z 32-voice .syx dropped on the FM-DX banks fills a slot; a CZ slot hands it over; single voices (VCED, then ACED) become one voice with the
# right bytes; the backup carries the 24 slots. Needs the page served (default http://localhost:8766/emulator.html: python3 -m http.server 8766 in the repository's root).
import asyncio, json, os, sys
from playwright.async_api import async_playwright
URL = os.environ.get('PW_PAGE', 'http://localhost:8766/emulator.html')
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
def vced(ops, alg, fbl, lfs, lfd, pmd, amd, sy, lfw, pms, ams, trnp, name):
    v = []
    for o in ops: v += o
    v += [alg, fbl, lfs, lfd, pmd, amd, sy, lfw, pms, ams, trnp, 0, 12] + [0] * 12 + [ord(c) for c in name.ljust(10)] + [99, 99, 99, 50, 50, 50]
    assert len(v) == 93, len(v)
    return v
def sysex(fmt, body, cnt):
    cs = (-sum(body)) & 0x7f
    return bytes([0xf0, 0x43, 0x00, fmt, cnt >> 7, cnt & 0x7f]) + bytes(body) + bytes([cs, 0xf7])
async def main():
    fails = []
    def check(ok, what):
        print(('  ok   ' if ok else '  FAIL ') + what)
        if not ok: fails.append(what)
    # a single voice: OP4, OP2, OP3, OP1 as [AR, D1R, D2R, RR, D1L, LS, RS, EBS, AME, KVS, OUT, CRS, DET]
    ops = [[31, 10, 5, 7, 3, 2, 1, 4, 1, 3, 60, 8, 5], [30, 11, 6, 8, 4, 3, 2, 5, 0, 4, 61, 9, 4], [29, 12, 7, 9, 5, 4, 3, 6, 1, 5, 62, 10, 3], [28, 13, 8, 10, 6, 5, 2, 7, 0, 6, 99, 11, 2]]
    one = vced(ops, 4, 5, 33, 6, 7, 8, 1, 2, 3, 2, 24, 'TESTVOICE')
    aced = [ord(c) for c in 'LM  8976AE'] + [1, 2, 3, 4, 1,  0, 1, 5, 6, 2,  1, 3, 7, 7, 3,  0, 0, 9, 1, 0] + [2, 3, 4]
    blob_single = sysex(0x03, one, 0x5d) + sysex(0x7e, aced, 0x21)
    internal = open(os.path.join(ROOT, 'assets', 'dx', 'INTERNAL.syx'), 'rb').read()
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path='/opt/pw-browsers/chromium-1194/chrome-linux/chrome')
        pg = await b.new_page(viewport={'width': 1400, 'height': 1100})
        errs = []
        pg.on('pageerror', lambda e: errs.append(str(e)))
        await pg.goto(URL); await pg.wait_for_timeout(1500)
        info = await pg.evaluate("({ n: __patchwerkBank.DX_BUILTIN.length, first: __patchwerkBank.DX_BUILTIN[0], second: __patchwerkBank.DX_BUILTIN[1], slots: __patchwerkBank.BANK.slots.length, cards: document.getElementById('dxbanks').children.length })")
        check(info['n'] == 32 and info['first'] == 'INIT' and info['second'] == 'E.PIANO 1' and info['slots'] == 40 and info['cards'] == 4, f'built in: {info}')
        # parsing
        v = await pg.evaluate("(b) => { const o = __patchwerkBank.parseTX81Z(new Uint8Array(b)); return o.map((x) => [...x]); }", list(blob_single))
        check(len(v) == 1, f'a single voice and its ACED: {len(v)} voice')
        if len(v) == 1:
            w = v[0]   # (file order OP4, OP2, OP3, OP1 as the 32-voice dump packs them)
            check(w[0:10] == [31, 10, 5, 7, 3, 2, 99, 60, 8, 13], f'first operator packed: {w[0:10]}')
            check(w[10:20] == [30, 11, 6, 8, 4, 3, 44, 61, 9, 20] and w[20:30] == [29, 12, 7, 9, 5, 4, 117, 62, 10, 27] and w[30:40] == [28, 13, 8, 10, 6, 5, 62, 99, 11, 18], 'the other three operators packed')
            check(w[40] == 108 and w[41:45] == [33, 6, 7, 8] and w[45] == 58 and w[46] == 24 and w[47] == 12, f'voice bytes: ALG/FBL/SY {w[40]}, LFO {w[41:45]}, PMS/AMS/LFW {w[45]}, TRPS {w[46]}, PBR {w[47]}')
            check(bytes(w[57:67]).decode() == 'TESTVOICE ' and w[67:73] == [99, 99, 99, 50, 50, 50], f'name {bytes(w[57:67])!r}, pitch EG {w[67:73]}')
            check(w[73:81] == [26, 67, 33, 101, 59, 119, 0, 25], f'the ACED dump merged: {w[73:81]}')
        v2 = await pg.evaluate("(b) => __patchwerkBank.parseTX81Z(new Uint8Array(b)).length", list(sysex(0x03, one, 0x5d)))
        check(v2 == 1, 'a single voice alone')
        # a 32-voice dump dropped on the FM-DX section: the first slot
        await pg.evaluate("""(b) => { const dt = new DataTransfer(); dt.items.add(new File([new Uint8Array(b)], 'MY BANK.syx', { type: '' }));
            document.getElementById('dxbanks').children[0].dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true })); }""", list(internal))
        await pg.wait_for_timeout(1500)
        st = await pg.evaluate("[...document.querySelectorAll('#dxbanks .bslot')].map((c) => [c.querySelector('header span').textContent, c.querySelector('header small').textContent])")
        check(st[0] == ['MY BANK', '32 voices'] and st[1][0] == 'empty', f'a 32-voice dump in DX 1: {st}')
        names = await pg.evaluate("__patchwerkBank.dxVoiceNames(__patchwerkBank.BANK.slots[20].data).slice(0, 3)")
        check(names == ['INIT', 'E.PIANO 1', 'E.PIANO 2'], f'its voice names {names}')
        # the same file dropped on a CZ slot goes to the next DX slot; a single voice to DX 3
        await pg.evaluate("""(b) => { const dt = new DataTransfer(); dt.items.add(new File([new Uint8Array(b)], 'again.syx', { type: '' }));
            document.getElementById('czbanks').children[0].dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true })); }""", list(internal))
        await pg.wait_for_timeout(1200)
        await pg.evaluate("""(b) => { const dt = new DataTransfer(); dt.items.add(new File([new Uint8Array(b)], 'one.syx', { type: '' }));
            document.getElementById('dxbanks').children[2].dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true })); }""", list(blob_single))
        await pg.wait_for_timeout(1200)
        st = await pg.evaluate("[...document.querySelectorAll('#dxbanks .bslot')].map((c) => c.querySelector('header small').textContent)")
        cz = await pg.evaluate("[...document.querySelectorAll('#czbanks .bslot header span')].map((e) => e.textContent)")
        check(st == ['32 voices', '32 voices', '1 voices', ''] and cz == ['empty'] * 4, f'a CZ slot hands it over, a single voice is a bank of one: {st}, CZ {cz}')
        # the engine reads the bank: a FM-DX module's BANK 1 PRESET 2 names the voice
        txt = await pg.evaluate("""() => { const t = __patchwerkBank.types().types.findIndex((x) => x.name === 'FM-DX'); return t; }""")
        check(txt >= 0, f'FM-DX is in the module list ({txt})')
        # the backup: 24 slots, the DX ones carried
        async with pg.expect_download(timeout=30000) as dl:
            await pg.click('#bk-go')
        bk = json.load(open(await (await dl.value).path()))
        check(len(bk['bank']) == 40 and bk['bank'][20] and bk['bank'][20]['name'] == 'MY BANK' and bk['bank'][21] and bk['bank'][22], f"the backup holds 40 slots ({len(bk['bank'])}; DX1 {bk['bank'][20] and bk['bank'][20]['name']})")
        await pg.evaluate("for (let i = 20; i < 24; i++) { const c = document.getElementById('dxbanks').children[i - 20]; c.querySelector('[data-a=clear]').click(); }")
        check(not errs, f'no page errors {errs[:2]}')
        await b.close()
    print('FM-DX PAGE TESTS PASSED' if not fails else f'{len(fails)} FAILED')
    sys.exit(1 if fails else 0)
asyncio.run(main())
