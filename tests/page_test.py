import asyncio, sys
from playwright.async_api import async_playwright
URL = 'http://localhost:8766/index.html'
async def main():
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path='/opt/pw-browsers/chromium-1194/chrome-linux/chrome',
            args=['--autoplay-policy=no-user-gesture-required'])
        pg = await b.new_page(viewport={'width': 1600, 'height': 1300})
        errs = []
        pg.on('console', lambda m: errs.append(f'{m.type}: {m.text}') if m.type in ('error','warning') else None)
        pg.on('pageerror', lambda e: errs.append(f'pageerror: {e}'))
        await pg.goto(URL); await pg.wait_for_timeout(800)
        await pg.screenshot(path='/tmp/shot_offline.png')
        n = await pg.evaluate("document.querySelectorAll('.mod').length")
        c = await pg.evaluate("document.querySelectorAll('#cables path.c').length")
        print('offline modules', n, 'cables', c)
        await pg.click('#power'); await pg.wait_for_timeout(2500)
        print('audio', await pg.evaluate("window.__patchwerkPeak()"))
        # play a key with the keyboard
        await pg.keyboard.down('g'); await pg.wait_for_timeout(500)
        await pg.screenshot(path='/tmp/shot_online.png')
        await pg.keyboard.up('g')
        # drag a cable: KEYS GATE? connect VCO SQR -> OUT R : find jacks
        await pg.evaluate("document.getElementById('bay').scrollIntoView()"); await pg.wait_for_timeout(300)
        src = pg.locator('.mod:has(header b:text("VCO 1")) .jack[data-dir=out][data-port="3"]')
        dst = pg.locator('.mod:has(header b:text("OUT 1")) .jack[data-dir=in][data-port="1"]')
        sb = await src.bounding_box(); db = await dst.bounding_box()
        await pg.mouse.move(sb['x']+8, sb['y']+8); await pg.mouse.down()
        await pg.mouse.move(db['x']+8, db['y']+8, steps=12); await pg.mouse.up()
        await pg.wait_for_timeout(400)
        print('cables after drag', await pg.evaluate("document.querySelectorAll('#cables path.c').length"))
        # factory load + play
        await pg.select_option('#factory', '1'); await pg.wait_for_timeout(500)
        await pg.click('#play-btn'); await pg.wait_for_timeout(1500); print('playing peak', await pg.evaluate('window.__patchwerkPeak()'))
        print('modules berlin', await pg.evaluate("document.querySelectorAll('.mod').length"))
        await pg.screenshot(path='/tmp/shot_berlin.png')
        await pg.evaluate("window.scrollTo(0, 0)"); await pg.wait_for_timeout(200)
        await pg.screenshot(path='/tmp/shot_top.png')
        await pg.set_viewport_size({'width': 400, 'height': 900}); await pg.wait_for_timeout(300)
        print('hscroll', await pg.evaluate("document.documentElement.scrollWidth"), )
        await pg.screenshot(path='/tmp/shot_mobile.png', full_page=False)
        print('\n'.join(errs) or 'no console errors')
        await b.close()
asyncio.run(main())
