"""Atlas -> straightened potrace outlines -> TrueType, in the metrics of the input font. Only the atlas glyphs.
    python build_font.py input/djiosd.ttf input/atlas.png out/djiosd.ttf"""
import sys, math
import numpy as np
from fontTools.ttLib import TTFont, newTable
from fontTools import subset
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.recordingPen import RecordingPen
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0
sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import straighten as st

SCALE = 25.6      # font units per atlas pixel (40 ppem at 1024 upm)
BASELINE = 34.0   # atlas row of the baseline
ORIGIN = 8.0      # atlas column of the glyph origin (the same in every cell)
SPACE = 409       # advance of the space (16 atlas px)

def to_font(p):
    return ((p.x / st.K - ORIGIN) * SCALE, (BASELINE - p.y / st.K) * SCALE)

def area(rec):
    pts = [a[-1] for op, a in rec.value if a]   # the end point of each segment
    return sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1])) / 2

def outline(path):
    """potrace path -> list of contours as RecordingPen in font units"""
    out = []
    for c in path:
        r = RecordingPen()
        r.moveTo(to_font(c.start_point))
        for s in c.segments:
            if s.is_corner:
                r.lineTo(to_font(s.c)); r.lineTo(to_font(s.end_point))
            else:
                r.curveTo(to_font(s.c1), to_font(s.c2), to_font(s.end_point))
        r.closePath()
        out.append(r)
    return out

def main(src, atlas, dst):
    font = TTFont(src)
    opts = subset.Options(); opts.notdef_outline = True; opts.layout_features = []; opts.name_IDs = ["*"]
    opts.drop_tables += ["EBDT", "EBLC", "EBSC", "kern"]
    sub = subset.Subsetter(opts); sub.populate(unicodes=range(32, 127)); sub.subset(font)
    glyf = font["glyf"]; hmtx = font["hmtx"]
    if 32 not in font.getBestCmap():   # the input font has no space: an empty glyph, 16 atlas px wide
        from fontTools.ttLib.tables._g_l_y_f import Glyph
        font.setGlyphOrder(font.getGlyphOrder() + ["space"])
        glyf.glyphs["space"] = Glyph(); hmtx["space"] = (SPACE, 0)
        for t in font["cmap"].tables:
            if t.isUnicode(): t.cmap[32] = "space"
        font["maxp"].numGlyphs = len(font.getGlyphOrder()); font["hhea"].numberOfHMetrics = 0
    cmap = font.getBestCmap()
    cell = st.load_atlas(atlas)
    for c in range(33, 127):
        name = cmap.get(c)
        if not name: continue
        contours = outline(st.glyph(cell(chr(c))))
        # TrueType: outer contours clockwise. potrace alternates outer/hole, so one test decides for the glyph
        rev = area(contours[0]) > 0 if contours else False
        pen = TTGlyphPen(None)
        cq = Cu2QuPen(pen, max_err=1.0, reverse_direction=rev)
        for r in contours: r.replay(cq)
        g = pen.glyph(); g.recalcBounds(glyf)
        glyf[name] = g
        hmtx[name] = (hmtx[name][0], g.xMin)
        print(chr(c), end="", flush=True)
    print()
    # kerning: adjacent digits touch (the rule of the original kern table), in whole atlas pixels
    pairs = {}
    digits = [cmap[c] for c in range(48, 58)]
    for a in digits:
        for b in digits:
            ga, gb = glyf[a], glyf[b]
            gap = hmtx[a][0] - ga.xMax + gb.xMin
            k = -round(gap / SCALE) * SCALE
            if round(k): pairs[(a, b)] = int(round(k))
    kern = newTable("kern"); kern.version = 0; t = KernTable_format_0(); t.coverage = 1; t.format = 0; t.kernTable = pairs
    t.version = 0; kern.kernTables = [t]; font["kern"] = kern
    font.save(dst)
    # optical side bearings for every glyph, then optical kerning of every pair (the bbox rule above leaves holes
    # after 7 and 4 and crowds 1; the input font's side bearings were uneven: f g h, .1)
    import space_optical
    pairs = space_optical.main(dst)
    print("saved", dst, len(pairs), "kern pairs")

if __name__ == "__main__":
    main(*sys.argv[1:4])
