"""Optical kerning of the digits: in the upright (de-slanted) shapes, the mean horizontal gap between two digits over
their height, with deep gaps capped (the open side of 7, 4), made equal to the gap of 00 unkerned.
    python kern_optical.py font.ttf [--write]"""
import sys, math, numpy as np
from fontTools.ttLib import TTFont
from fontTools.pens.basePen import BasePen
SLANT = 5 / 22          # dx/dy of the stems
STEP = 8                # units between the measured rows
CAP = 1.4               # gaps deeper than CAP x the reference gap count as CAP x
MIN_GAP = 0.7           # and no row may come closer than MIN_GAP x the reference gap (no collisions)
ROUND = 5               # kerning rounded to this many units

class Poly(BasePen):
    def __init__(s, gs): super().__init__(gs); s.polys = []; s.cur = []
    def _moveTo(s, p): s.cur = [p]
    def _lineTo(s, p): s.cur.append(p)
    def _qCurveToOne(s, a, b):
        p0 = s.cur[-1]; s.cur += [tuple((1-t)**2*p0[k] + 2*(1-t)*t*a[k] + t**2*b[k] for k in (0, 1)) for t in np.linspace(0, 1, 9)[1:]]
    def _curveToOne(s, a, b, c):
        p0 = s.cur[-1]; s.cur += [tuple((1-t)**3*p0[k] + 3*(1-t)**2*t*a[k] + 3*(1-t)*t**2*b[k] + t**3*c[k] for k in (0, 1)) for t in np.linspace(0, 1, 9)[1:]]
    def _closePath(s): s.polys.append(s.cur); s.cur = []

def profiles(gs, name, ys):
    """left and right edge of the de-slanted glyph at each y (nan where the row is empty)"""
    p = Poly(gs); gs[name].draw(p)
    L = np.full(len(ys), np.nan); Rr = np.full(len(ys), np.nan)
    for poly in p.polys:
        pts = [(x - y * SLANT, y) for x, y in poly]
        for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]):
            if y0 == y1: continue
            for i, y in enumerate(ys):
                if (y0 <= y < y1) or (y1 <= y < y0):
                    x = x0 + (y - y0) * (x1 - x0) / (y1 - y0)
                    L[i] = np.nanmin([L[i], x]); Rr[i] = np.nanmax([Rr[i], x])
    return L, Rr

def main(path, write):
    f = TTFont(path); gs = f.getGlyphSet(); cmap = f.getBestCmap(); hmtx = f["hmtx"]
    names = [cmap[c] for c in range(48, 58)]
    g0 = f["glyf"][names[0]]; ys = np.arange(g0.yMin + STEP / 2, g0.yMax, STEP)
    prof = {n: profiles(gs, n, ys) for n in names}
    def gaps(a, b):
        return hmtx[a][0] - prof[a][1] + prof[b][0]       # per row, without kerning
    ref = np.nanmean(gaps(names[0], names[0]))
    def measure(a, b, k):
        g = gaps(a, b) + k
        g = np.where(np.isnan(g), CAP * ref, np.minimum(g, CAP * ref))
        return g.mean()
    pairs = {}
    for a in names:
        for b in names:
            lo, hi = -400.0, 400.0
            for _ in range(40):
                mid = (lo + hi) / 2
                if measure(a, b, mid) < ref: lo = mid
                else: hi = mid
            kmin = MIN_GAP * ref - np.nanmin(gaps(a, b))      # the closest row decides how tight it may get
            k = int(round(max(mid, kmin) / ROUND) * ROUND)
            if k: pairs[(a, b)] = k
    print("reference gap (00) %.0f units; %d pairs" % (ref, len(pairs)))
    if write:
        f["kern"].kernTables[0].kernTable = pairs; f.save(path); print("written to", path)
    return pairs

if __name__ == "__main__":
    main(sys.argv[1], "--write" in sys.argv)
