"""Optical spacing and kerning, in the upright (de-slanted) shapes.
Gap of a pair = mean horizontal distance between the facing edges over the rows where both glyphs have ink, deep
gaps capped at CAP x the reference gap, and no row closer than MIN_GAP x the reference gap.
1. spacing: each glyph's left and right side bearings so that its gap to the reference glyph (H for capitals,
   digits and symbols, n for lowercase) on either side equals the reference's gap to itself;
2. kerning: every pair, the same measure made equal to the reference gap; kept when |k| >= KEEP units.
    python space_optical.py font.ttf"""
import sys, numpy as np
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0
from fontTools.ttLib import newTable
from kern_optical import Poly, SLANT

STEP, CAP, MIN_GAP, ROUND, KEEP = 8, 1.4, 0.7, 5, 15
CAP_HEIGHT = 538

def main(path):
    f = TTFont(path); gs = f.getGlyphSet(); cmap = f.getBestCmap(); hmtx = f["hmtx"]; glyf = f["glyf"]
    ys = np.arange(-200, 760, STEP) + STEP / 2
    def profile(name):
        p = Poly(gs); gs[name].draw(p)
        L = np.full(len(ys), np.nan); R = np.full(len(ys), np.nan)
        for poly in p.polys:
            pts = [(x - y * SLANT, y) for x, y in poly]
            for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]):
                if y0 == y1: continue
                lo, hi = min(y0, y1), max(y0, y1)
                for i in np.nonzero((ys >= lo) & (ys < hi))[0]:
                    x = x0 + (ys[i] - y0) * (x1 - x0) / (y1 - y0)
                    L[i] = np.nanmin([L[i], x]); R[i] = np.nanmax([R[i], x])
        # within the cap band, a row without ink counts as the glyph's nearest edge: small marks (. , : ' -)
        # are spaced as if they were a bar of their own width, and what sticks out next to them (the flag of 1)
        # is seen
        band = (ys >= 0) & (ys <= CAP_HEIGHT)
        if np.any(~np.isnan(L)):
            L = np.where(np.isnan(L) & band, np.nanmin(L), L); R = np.where(np.isnan(R) & band, np.nanmax(R), R)
        return L, R
    names = {c: cmap[c] for c in range(33, 127)}
    prof = {n: profile(n) for n in names.values()}
    adv = {n: hmtx[n][0] for n in names.values()}
    def raw_gaps(a, b): return adv[a] - prof[a][1] + prof[b][0]      # nan where a row is empty in either
    def fit(a, b, target):
        """the extra space k so that the pair's measure equals target (and no row gets too close)"""
        g = raw_gaps(a, b)
        if np.all(np.isnan(g)): return None
        lo, hi = -600.0, 600.0
        for _ in range(40):
            mid = (lo + hi) / 2
            m = np.nanmean(np.minimum(g + mid, CAP * target))
            if m < target: lo = mid
            else: hi = mid
        return max(mid, MIN_GAP * target - np.nanmin(g))
    # 1. spacing against the references
    def ref_of(c): return names[ord("n")] if chr(c).islower() else names[ord("H")]
    targets = {r: np.nanmean(raw_gaps(r, r)) for r in {names[ord("H")], names[ord("n")]}}
    shifts = {}
    for c, n in names.items():
        r = ref_of(c)
        if n == r: continue
        left, right = fit(r, n, targets[r]), fit(n, r, targets[r])
        if left is None or right is None: continue
        shifts[n] = (int(round(left)), int(round(right)))
    for n, (sl, sr) in shifts.items():
        g = glyf[n]
        if g.numberOfContours:
            coords, _, _ = g.getCoordinates(glyf); g.coordinates.translate((sl, 0)); g.recalcBounds(glyf)
        adv[n] = adv[n] + sl + sr; hmtx[n] = (adv[n], g.xMin if g.numberOfContours else 0)
        L, R = prof[n]; prof[n] = (L + sl, R + sl)
    # 2. kerning: every pair against the target of the left glyph's class
    pairs = {}
    for ca, a in names.items():
        for cb, b in names.items():
            t = targets[ref_of(ca)] if chr(ca).islower() == chr(cb).islower() else min(targets.values())
            k = fit(a, b, t)
            if k is None: continue
            k = int(round(k / ROUND) * ROUND)
            if abs(k) >= KEEP: pairs[(a, b)] = k
    kern = newTable("kern"); kern.version = 0; t = KernTable_format_0(); t.coverage = 1; t.format = 0; t.version = 0
    t.kernTable = pairs; kern.kernTables = [t]; f["kern"] = kern
    f.save(path)
    print("spacing: %d glyphs re-spaced (targets %s); kerning: %d pairs" % (len(shifts), {k: round(v) for k, v in targets.items()}, len(pairs)))
    return pairs

if __name__ == "__main__":
    main(sys.argv[1])
