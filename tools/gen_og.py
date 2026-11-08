#!/usr/bin/env python3
"""brand/og.png: the 1200x630 preview image link cards (Facebook, Discord, Slack...) show: the logo and the patch screen."""
from pathlib import Path
from PIL import Image
root = Path(__file__).resolve().parent.parent
W, H = 1200, 630
bg = Image.new('RGB', (W, H), (0, 0, 0))
shot = Image.open(root / 'web/shots/cable.png').convert('RGB').resize((410, 410), Image.NEAREST)
logo = Image.open(root / 'brand/logo.png').convert('RGB')
logo = logo.resize((logo.width * 3 // 4, logo.height * 3 // 4), Image.NEAREST)
pad = (H - 410) // 2
bg.paste(shot, (W - 410 - 50, pad))
bg.paste(logo, (50, (H - logo.height) // 2))
bg.save(root / 'brand/og.png', optimize=True)
print('brand/og.png', bg.size)
