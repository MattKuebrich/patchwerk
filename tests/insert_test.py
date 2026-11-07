# The library dragged onto the patch (web/page.html): onto a cable inserts the module into it, onto a jack wires it
# to that jack, onto empty space places it on its own (pw_add_wired). Needs the page served on :8766.
import asyncio, re
from playwright.async_api import async_playwright
URL = 'http://localhost:8766/index.html'
async def main():
    fails = 0
    def check(ok, what):
        nonlocal fails
        print(('  ok   ' if ok else '  FAIL ') + what)
        if not ok: fails += 1
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path='/opt/pw-browsers/chromium-1194/chrome-linux/chrome')
        pg = await b.new_page(viewport={'width': 1600, 'height': 1300})
        errs = []
        pg.on('pageerror', lambda e: errs.append(str(e)))
        await pg.goto(URL); await pg.wait_for_timeout(800)
        await pg.evaluate("(() => { const N = window.__patchwerk; N.call('pw_web_clear'); const v = N.callRet('pw_web_add', 0, 60, 60), o = N.callRet('pw_web_add', 25, 700, 60); N.call('pw_web_connect', v, 2, o, 0); })()")
        await pg.wait_for_timeout(300)
        await pg.evaluate("document.getElementById('bay').scrollIntoView()"); await pg.wait_for_timeout(300)
        count = lambda: pg.evaluate("[document.querySelectorAll('.mod').length, document.querySelectorAll('#cables path.c').length]")
        check(await count() == [2, 1], 'a VCO into the OUTPUT')
        async def drag(name, to):
            item = pg.locator('.lib-item', has_text=re.compile('^' + name)).first
            await item.scroll_into_view_if_needed()
            ib = await item.bounding_box()
            await pg.mouse.move(ib['x'] + 20, ib['y'] + 8); await pg.mouse.down()
            await pg.mouse.move(to[0], to[1], steps=15)
            hl = await pg.evaluate("[document.querySelectorAll('#cables path.c.ins').length, document.querySelectorAll('.jack.hot').length]")
            await pg.mouse.up(); await pg.wait_for_timeout(400)
            return hl
        mid = await pg.evaluate("""(() => { const c = document.querySelector('#cables path.c'), l = c.getTotalLength(), q = c.getPointAtLength(l / 2),
            m = c.getScreenCTM(); return [q.x * m.a + q.y * m.c + m.e, q.x * m.b + q.y * m.d + m.f]; })()""")
        hl = await drag('Vcf', mid)
        check(hl == [1, 0], 'the cable lit while over it')
        check(await count() == [3, 2], 'VCF dropped on the cable: inserted (3 modules, 2 cables)')
        names = await pg.evaluate("(() => { const P = window.__patchwerk.patch(); return JSON.stringify(P.cables.map((c) => [P.modules.find((m) => m.slot === c[1]).label, P.modules.find((m) => m.slot === c[3]).label])); })()")
        check('VCF' in names and 'VCO' in names and 'OUT' in names, 'VCO > VCF > OUT ' + names)
        jb = await pg.locator('.mod:has(header b:text("VCF 1")) .jack[data-dir=out][data-port="0"]').bounding_box()
        hl = await drag('Delay', (jb['x'] + 8, jb['y'] + 8))
        check(hl[1] == 1, 'the jack lit while over it')
        check(await count() == [4, 3], 'DELAY dropped on VCF OUT (cabled): inserted after it')
        cr = await pg.evaluate("(() => { const r = document.getElementById('canvas') ? document.getElementById('canvas').getBoundingClientRect() : null; return r && [r.left, r.top, r.width, r.height]; })()")
        hl = await drag('Lfo', (cr[0] + 60, cr[1] + cr[3] - 60) if cr else (900, 1200))
        check(await count() == [5, 3], 'LFO dropped on empty space: alone')
        await pg.evaluate("window.__patchwerk.call('pw_web_undo')"); await pg.wait_for_timeout(300)
        await pg.evaluate("window.__patchwerk.call('pw_web_undo')"); await pg.wait_for_timeout(300)
        check(await count() == [3, 2], 'one undo per drop')
        await pg.screenshot(path='tests/out/insert.png')
        check(not errs, 'no page errors ' + str(errs))
        await b.close()
    print('INSERT TESTS PASSED' if not fails else f'{fails} INSERT TESTS FAILED')
asyncio.run(main())
