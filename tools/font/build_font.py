"""Atlas -> straight lines + fitted curves (fitbuild.py) -> TrueType in the metrics of the input font, then optical
spacing and kerning. Only the atlas glyphs.
    python build_font.py djiosd-traced.ttf atlas.png ../../assets/djiosd.ttf [OUTLINES.ttf]
OUTLINES.ttf: the outlines before spacing, aligned with the atlas: python score3.py atlas.png OUTLINES.ttf"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from fontTools.ttLib import TTFont
from fontTools import subset
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.ttLib.tables._g_l_y_f import Glyph
import straighten as st, fitbuild as fb, space_optical

SCALE, BASELINE, ORIGIN, SPACE = 25.6, 34.0, 8.0, 409

def F(p): return ((p[0] / st.K - ORIGIN) * SCALE, (BASELINE - p[1] / st.K) * SCALE)

def poly(segs):
    out = []
    for s in segs:
        out += [s[1], s[2]] if s[0] == "line" else list(fb.bez(np.array(s[1:5]), np.linspace(0, 1, 12)))
    return np.array([F(p) for p in out])

def area(P): return 0.5 * np.sum(P[:, 0] * np.roll(P[:, 1], -1) - np.roll(P[:, 0], -1) * P[:, 1])

def inside(pt, P):
    x, y = pt; c = False
    for (x0, y0), (x1, y1) in zip(P, np.roll(P, -1, 0)):
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0): c = not c
    return c

def draw(pen, segs, reverse):
    if reverse:
        r = []
        for s in reversed(segs):
            r.append(("line", s[2], s[1]) if s[0] == "line" else ("cubic", s[4], s[3], s[2], s[1]))
        segs = r
    pen.moveTo(F(segs[0][1]))
    for s in segs:
        if s[0] == "line": pen.lineTo(F(s[2]))
        else: pen.curveTo(F(s[2]), F(s[3]), F(s[4]))
    pen.closePath()

def main(src, atlas, dst, outlines=None):
    font = TTFont(src)
    opts = subset.Options(); opts.notdef_outline = True; opts.layout_features = []; opts.name_IDs = ["*"]
    opts.drop_tables += ["EBDT", "EBLC", "EBSC", "kern"]
    sub = subset.Subsetter(opts); sub.populate(unicodes=range(32, 127)); sub.subset(font)
    glyf = font["glyf"]; hmtx = font["hmtx"]
    if 32 not in font.getBestCmap():
        font.setGlyphOrder(font.getGlyphOrder() + ["space"]); glyf.glyphs["space"] = Glyph(); hmtx["space"] = (SPACE, 0)
        for t in font["cmap"].tables:
            if t.isUnicode(): t.cmap[32] = "space"
        font["maxp"].numGlyphs = len(font.getGlyphOrder()); font["hhea"].numberOfHMetrics = 0
    cmap = font.getBestCmap(); cell = st.load_atlas(atlas)
    for c in range(33, 127):
        name = cmap[c]; ch = chr(c)
        cons, _ = fb.glyph_contours(cell(ch), ch)
        cons = [s for s in cons if len(s) >= 2]
        polys = [poly(s) for s in cons]
        pen = TTGlyphPen(None); cq = Cu2QuPen(pen, max_err=1.0)
        for i, (segs, P) in enumerate(zip(cons, polys)):
            depth = sum(inside(P[0], Q) for j, Q in enumerate(polys) if j != i)
            want_cw = depth % 2 == 0                      # TrueType: outer contours clockwise
            draw(cq, segs, (area(P) < 0) != want_cw)
        g = pen.glyph(); g.recalcBounds(glyf); glyf[name] = g; hmtx[name] = (hmtx[name][0], g.xMin)
        print(ch, end="", flush=True)
    print()
    font.save(dst)
    if outlines: font.save(outlines)   # before spacing: aligned with the atlas, for score3.py
    pairs = space_optical.main(dst)
    print("saved", dst, len(pairs), "kern pairs")

if __name__ == "__main__":
    main(*sys.argv[1:5])
