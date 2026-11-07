# The page says plainly when it is not linked to an FM-1: a banner with a Link button, shown until linked.
import asyncio
from playwright.async_api import async_playwright
URL = 'http://localhost:8766/index.html'
fails = 0
def check(c, m):
    global fails
    print(('  ok   ' if c else '  FAIL ') + m)
    if not c: fails += 1
async def main():
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path='/opt/pw-browsers/chromium-1194/chrome-linux/chrome')
        pg = await b.new_page(viewport={'width': 1300, 'height': 1000})
        await pg.goto(URL); await pg.wait_for_timeout(1800)
        check(await pg.is_visible('#link-banner'), 'the banner shows when nothing is linked')
        txt = await pg.text_content('#link-banner-text')
        check('Not linked' in txt or 'plugged in' in txt or 'no Web MIDI' in txt, f'it says why nothing syncs: {txt[:70]}')
        check(await pg.is_visible('#link-banner-btn') or 'no Web MIDI' in txt, 'a Link FM-1 button right in it')
        await b.close()
    print('BANNER TESTS ' + ('FAILED' if fails else 'PASSED')); raise SystemExit(1 if fails else 0)
asyncio.run(main())
