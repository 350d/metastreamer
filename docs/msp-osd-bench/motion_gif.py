#!/usr/bin/env python3
"""motion_gif.py LEFT.pkl RIGHT.pkl HZ OUT.gif: two runs side by side, 20 snapshots (10 s at 2/s) played at 250 ms.
Left (master): the 10 s with the most elements missing, by lit cells. Right (PR): the 10 s with the most roll."""
import io, pickle, sys
from PIL import Image, ImageDraw, ImageFont

src = open(__file__.replace('motion_gif.py', 'motion_usb.py')).read()
exec(src[src.index('def lit_cells'):src.index('def msp')])   # lit_cells() of the motion scripts

left, right, hz, out = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
N, W, H, GAP = 20, 638, 360, 4
L = pickle.load(open(left, 'rb')); R = pickle.load(open(right, 'rb'))

lit = [len(lit_cells(Image.open(io.BytesIO(b)).convert('L'))) for _, b in L['snaps']]
i0 = min(range(len(lit) - N + 1), key=lambda i: sum(lit[i:i + N]))

def roll_span(i):
    t0, t1 = R['snaps'][i][0], R['snaps'][i + N - 1][0]
    r = [a[1] for a in R['att'] if t0 <= a[0] <= t1]
    return (max(r) - min(r)) if r else 0
j0 = max(range(len(R['snaps']) - N + 1), key=roll_span)

try: font = ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc', 22)
except Exception: font = ImageFont.load_default()

def label(im, text, colour):
    d = ImageDraw.Draw(im, 'RGBA'); x, y = 8, H - 44
    w = d.textlength(text, font=font)
    d.rectangle((x, y, x + w + 12, y + 32), fill=(0, 0, 0, 170)); d.text((x + 6, y + 3), text, font=font, fill=colour)

frames = []
for k in range(N):
    f = Image.new('RGB', (2 * W + GAP, H), (110, 110, 110))
    a = Image.open(io.BytesIO(L['snaps'][i0 + k][1])).convert('RGB').resize((W, H))
    b = Image.open(io.BytesIO(R['snaps'][j0 + k][1])).convert('RGB').resize((W, H))
    label(a, f'master, {hz} Hz', (240, 110, 100)); label(b, f'this PR, {hz} Hz', (140, 230, 140))
    f.paste(a, (0, 0)); f.paste(b, (W + GAP, 0)); frames.append(f)
frames[0].save(out, save_all=True, append_images=frames[1:], duration=250, loop=0, optimize=True)
print(out, 'left from', i0, 'lit', min(lit[i0:i0 + N]), '-', max(lit[i0:i0 + N]), '; right from', j0, 'roll span', roll_span(j0) / 10, 'deg')
