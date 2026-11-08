#!/usr/bin/env python3
"""brand/og.png: the 1200x630 preview image link cards (Facebook, Discord, Slack...) show:
the patch screen, the logo (the power-on screen, brand/logo.png) and MOD SEQ's step editor (brand/mseq.png: the host simulator's shot)."""
from pathlib import Path
from PIL import Image
root = Path(__file__).resolve().parent.parent
W, H, S, GAP = 1200, 630, 320, 36
bg = Image.new('RGB', (W, H), (0, 0, 0))
patch = Image.open(root / 'web/shots/cable.png').convert('RGB').resize((S, S), Image.LANCZOS)
mseq = Image.open(root / 'brand/mseq.png').convert('RGB').resize((S, S), Image.LANCZOS)
logo = Image.open(root / 'brand/logo.png').convert('RGB'); logo = logo.resize((logo.width // 2, logo.height // 2), Image.NEAREST)
total = 2 * S + logo.width + 2 * GAP
x = (W - total) // 2
bg.paste(patch, (x, (H - S) // 2)); x += S + GAP
bg.paste(logo, (x, (H - logo.height) // 2)); x += logo.width + GAP
bg.paste(mseq, (x, (H - S) // 2))
bg.save(root / 'brand/og.png', optimize=True)
print('brand/og.png', bg.size)
