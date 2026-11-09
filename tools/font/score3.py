"""Coverage error of the font OUTLINES (no hinting) against the atlas at its own resolution.
    python score3.py atlas.png font.ttf [font2.ttf ...]"""
import sys, numpy as np
from PIL import Image, ImageDraw
from fontTools.ttLib import TTFont
from fontTools.pens.basePen import BasePen
SC, OX, BY, R = 25.6, 8.0, 34.0, 32
A = np.asarray(Image.open(sys.argv[1]).getchannel("A"), dtype=float) / 255
def cell(c): c -= 32; return A[c // 16 * 42:c // 16 * 42 + 42, c % 16 * 46:c % 16 * 46 + 46]
class Poly(BasePen):
    def __init__(s, gs): super().__init__(gs); s.polys = []; s.cur = []
    def _moveTo(s, p): s.cur = [p]
    def _lineTo(s, p): s.cur.append(p)
    def _curveToOne(s, a, b, c):
        p0 = s.cur[-1]
        s.cur += [tuple((1-t)**3*p0[k] + 3*(1-t)**2*t*a[k] + 3*(1-t)*t**2*b[k] + t**3*c[k] for k in (0, 1)) for t in np.linspace(0, 1, 16)[1:]]
    def _qCurveToOne(s, a, b):
        p0 = s.cur[-1]
        s.cur += [tuple((1-t)**2*p0[k] + 2*(1-t)*t*a[k] + t**2*b[k] for k in (0, 1)) for t in np.linspace(0, 1, 16)[1:]]
    def _closePath(s): s.polys.append(s.cur); s.cur = []
def errors(path):
    tt = TTFont(path); gs = tt.getGlyphSet(); cmap = tt.getBestCmap(); out = {}
    for c in range(33, 127):
        p = Poly(gs); gs[cmap[c]].draw(p); m = np.zeros((42 * R, 46 * R), bool)
        for poly in p.polys:
            t = Image.new("1", (46 * R, 42 * R), 0)
            ImageDraw.Draw(t).polygon([((x / SC + OX) * R, (BY - y / SC) * R) for x, y in poly], fill=1); m ^= np.asarray(t)
        d = np.abs(m.reshape(42, R, 46, R).mean((1, 3)) - cell(c))
        # the atlas stores alpha in 15 levels: each pixel is only known to +-1/30
        out[chr(c)] = (d.sum() / cell(c).sum(), np.maximum(d - 1 / 30, 0).sum() / cell(c).sum())
    return out
for p in sys.argv[2:]:
    r = errors(p)
    for i, label in ((0, "raw"), (1, "beyond quantization")):
        v = np.array([x[i] for x in r.values()])
        print("%-26s %-20s mean %.2f%% median %.2f%%  <1%%: %d/94  worst: %s" % (p, label, 100 * v.mean(), 100 * np.median(v), (v < 0.01).sum(),
              " ".join(f"{k}{100*x[i]:.1f}" for k, x in sorted(r.items(), key=lambda t: -t[1][i])[:12])))
