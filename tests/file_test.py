import asyncio
from playwright.async_api import async_playwright
async def main():
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path='/opt/pw-browsers/chromium-1194/chrome-linux/chrome', args=['--autoplay-policy=no-user-gesture-required'])
        pg = await b.new_page()
        pg.on('pageerror', lambda e: print('pageerror', e))
        await pg.goto('file://' + __import__('os').path.abspath(__import__('os').path.join(__import__('os').path.dirname(__file__), '..', 'emulator.html'))); await pg.wait_for_timeout(1000)
        print('modules', await pg.evaluate("document.querySelectorAll('.mod').length"))
        await pg.click('#power'); await pg.wait_for_timeout(1500)
        await pg.keyboard.down('g'); await pg.wait_for_timeout(600)
        print('audio', await pg.evaluate("window.__patchwerkPeak()"))
        await b.close()
asyncio.run(main())
