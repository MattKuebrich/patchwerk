# Build a voice using only the emulated FM-1 panel, under the strictest policy (JavaScript engine + main-thread
# audio), and check for sound: ARP + SEL a new patch, FX + SEL adds KEYS and a VCO, SEL plugs the cables.
import asyncio
from playwright.async_api import async_playwright
CSP = "script-src 'unsafe-inline'; worker-src 'none'"
async def main():
    html = open(__import__('os').path.join(__import__('os').path.dirname(__file__), '..', 'emulator.html')).read()
    open('/home/claude/m/out/patchwerk/web/build/csp.html','w').write(f'<meta http-equiv="Content-Security-Policy" content="{CSP}">' + html)
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path='/opt/pw-browsers/chromium-1194/chrome-linux/chrome', args=['--autoplay-policy=no-user-gesture-required'])
        pg = await b.new_page(viewport={'width':1600,'height':1200})
        pg.on('pageerror', lambda e: print('pageerror', e))
        await pg.goto('http://localhost:8766/csp.html'); await pg.wait_for_timeout(1200)
        async def click(loc):
            bb = await loc.bounding_box(); await pg.mouse.move(bb['x']+bb['width']/2, bb['y']+bb['height']*0.6)
            await pg.mouse.down(); await pg.wait_for_timeout(110); await pg.mouse.up(); await pg.wait_for_timeout(140)
        btn = lambda n: click(pg.locator(f'.fbtn[aria-label^="{n} "]'))
        dials = pg.locator('.dial')
        async def turn(idx, n):
            for _ in range(abs(n)):
                await dials.nth(idx).dispatch_event('wheel', {'deltaY': -100 if n > 0 else 100}); await pg.wait_for_timeout(45)
        await pg.click('#power'); await pg.wait_for_timeout(1500)
        await turn(2, 1); await turn(1, -20); await btn('SEL')                 # PRESETS: the patch list; a new patch (an OUTPUT)
        await btn('FX'); await turn(1, 60); await turn(1, -3); await btn('ENV')  # KEYS (before AUDIO IN, MIDI IN, MIDI OUT), unconnected
        await btn('FX'); await turn(1, -60); await btn('SEL')                  # VCO, wired from KEYS PITCH (under the cursor) to its V/OCT
        print('modules', await pg.evaluate("document.querySelectorAll('.mod').length"))
        await turn(1, 2)                                                       # (the cursor on its SIN) TRI -> SAW
        await btn('SEL'); await btn('SEL')                                     # pick up SAW (hops to OUT L), plug
        print('cables', await pg.evaluate("document.querySelectorAll('#cables path.c').length"), await pg.evaluate("[...document.querySelectorAll('.mod header b')].map(b=>b.textContent).join(',')"))
        await pg.locator('#lcd').screenshot(path='lcd/flow_end.png')
        await pg.keyboard.down('g'); await pg.wait_for_timeout(600)
        print('peak while key held:', await pg.evaluate("window.__patchwerkPeak()"))
        await pg.keyboard.up('g')
        # GLO tapped: the shortcuts on screen
        await btn('GLO'); await pg.wait_for_timeout(200)
        await pg.locator('#lcd').screenshot(path='lcd/help.png')
        await b.close()
asyncio.run(main())
