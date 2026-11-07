import asyncio, sys
from playwright.async_api import async_playwright
CSP = sys.argv[1] if len(sys.argv) > 1 else "script-src 'unsafe-inline' https://cdnjs.cloudflare.com"
async def main():
    html = open(__import__('os').path.join(__import__('os').path.dirname(__file__), '..', 'emulator.html')).read()
    html = f'<meta http-equiv="Content-Security-Policy" content="{CSP}">' + html
    open('/home/claude/m/out/patchwerk/web/build/csp.html','w').write(html)
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path='/opt/pw-browsers/chromium-1194/chrome-linux/chrome', args=['--autoplay-policy=no-user-gesture-required'])
        pg = await b.new_page(viewport={'width':1600,'height':1200})
        pg.on('pageerror', lambda e: print('pageerror', e))
        pg.on('console', lambda m: print('console', m.type, m.text[:200]) if 'TUNNEL' not in m.text else None)
        await pg.goto('http://localhost:8766/csp.html'); await pg.wait_for_timeout(1500)
        print('modules', await pg.evaluate("document.querySelectorAll('.mod').length"))
        if await pg.evaluate("!!document.getElementById('power')"):
            await pg.click('#power'); await pg.wait_for_timeout(2000)
        print('state', await pg.evaluate("[document.getElementById('power-label')?.textContent, document.getElementById('lcd-off')?.hidden, window.__patchwerkAudio||null]"))
        await b.close()
asyncio.run(main())
