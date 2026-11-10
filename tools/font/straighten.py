"""Atlas cell -> potrace -> straighten long nearly-straight runs (one least-squares line each, stems at one global
slant) -> rasterize -> potrace again. Coordinates: atlas pixels * K (y down)."""
import sys, math, numpy as np, potrace
from PIL import Image, ImageDraw, ImageFilter, ImageOps

K = 16
SIGMA = 0.0           # px of the atlas: removes the pixel staircase before the first trace
RUN_MIN = 1.6         # px: a straight run at least this long becomes one exact line...
RUN_LONG = 4.5        # px: ...and shorter than this only between two corners (else it is a chord of a curve)
CORNER_LOOK, CORNER_TURN = 1.5, 25   # px, deg: a corner = the contour turns this much within that distance
EPS = 0.15            # px: max deviation of a run from its line
SAG_MAX = 0.10        # px: residual bow above this = a round, not a straight
TRIM = 0.6            # px: kept off the ends of each straight run (they belong to the curves next to it)
SLANT = math.atan(5 / 22)   # stems: dx/dy
STEM_TOL = 0.0   # the atlas stems already share one slant (12.76 +- 0.28 deg): snapping only costs fidelity

def load_atlas(path):
    a = Image.open(path).getchannel("A")
    return lambda ch: a.crop(((ord(ch) - 32) % 16 * 46, (ord(ch) - 32) // 16 * 42,
                              (ord(ch) - 32) % 16 * 46 + 46, (ord(ch) - 32) // 16 * 42 + 42))

def P(p): return np.array((p.x, p.y))

def dense(path, step=2.0):
    """contours as dense point arrays (units: K-scaled px)"""
    out = []
    for c in path:
        pts = [P(c.start_point)]
        for s in c.segments:
            if s.is_corner:
                for q in (P(s.c), P(s.end_point)):
                    a = pts[-1]; n = max(1, int(np.linalg.norm(q - a) / step))
                    pts += [a + (q - a) * t for t in np.linspace(0, 1, n + 1)[1:]]
            else:
                p0, c1, c2, p3 = pts[-1], P(s.c1), P(s.c2), P(s.end_point)
                n = max(4, int((np.linalg.norm(c1 - p0) + np.linalg.norm(c2 - c1) + np.linalg.norm(p3 - c2)) / step))
                for t in np.linspace(0, 1, n + 1)[1:]:
                    pts.append((1-t)**3*p0 + 3*(1-t)**2*t*c1 + 3*(1-t)*t**2*c2 + t**3*p3)
        out.append(np.array(pts[:-1]))
    return out

def fit_line(pts):
    m = pts.mean(0); u, s, vt = np.linalg.svd(pts - m); d = vt[0]
    return m, d, np.abs((pts - m) @ np.array((-d[1], d[0]))).max()

def bowed(pts):
    """a large-radius round fits a line within EPS over a few px, but its residuals bow to one side (a parabola);
    the pixel staircase on a straight edge only wobbles around zero"""
    m, d, _ = fit_line(pts)
    t = (pts - m) @ d; r = (pts - m) @ np.array((-d[1], d[0]))
    a = np.polyfit(t, r, 2)[0]
    sag = abs(a) * ((t.max() - t.min()) / 2) ** 2
    return sag > SAG_MAX * K

def corner_end(pts, idx, either=False, deg=None):
    """a short straight is bounded by corners on both sides; a chord of a curve turns gently at one end at least"""
    n = len(pts); m = max(2, int(CORNER_LOOK * K / 2))      # dense points are ~2 K-px apart
    d = pts[idx[-1]] - pts[idx[0]]; d = d / np.linalg.norm(d)
    def turn(a, b):
        v = pts[b % n] - pts[a % n]; v = v / (np.linalg.norm(v) or 1)
        return math.degrees(math.acos(max(-1, min(1, v @ d))))
    return (max if either else min)(turn(idx[-1], idx[-1] + m), turn(idx[0] - m, idx[0])) > (deg or CORNER_TURN)

SLANT_RUN, SLANT_TOL, SLANT_CORNER = 2.0, 4.0, 60   # px, degrees: a short run along the italic slant with a
# sharp corner at one end is a cut: a terminal (j), the end of a bar (T)

def slanted(pts, idx):
    d = pts[idx[-1]] - pts[idx[0]]
    if np.linalg.norm(d) < SLANT_RUN * K: return False
    ang = math.degrees(math.atan2(-d[0], d[1]) if d[1] >= 0 else math.atan2(d[0], -d[1]))
    return abs(ang - math.degrees(SLANT)) < SLANT_TOL and corner_end(pts, idx, either=True, deg=SLANT_CORNER)

CHORD_MAX, CHORD_H, CHORD_S, CHORD_CORNER = 5.0, 8.0, 5.0, 40   # px, degrees

def chord(pts, idx):
    """a flat stretch of a wide round (the shoulder of G, C, S): short, oblique (neither horizontal nor along
    the slant) and running smoothly into the curves on both sides"""
    d = pts[idx[-1]] - pts[idx[0]]
    if np.linalg.norm(d) >= CHORD_MAX * K or corner_end(pts, idx, either=True, deg=CHORD_CORNER): return False
    ang = math.degrees(math.atan2(d[0], d[1])) % 180          # 90 = horizontal
    slant = 180 - math.degrees(SLANT)
    return abs(ang - 90) > CHORD_H and abs(ang - slant) > CHORD_S

def straighten(pts, runs=None):
    """grow maximal runs whose points stay within EPS of a line; replace each long run by its projection on the
    fitted line (stems snapped to the global slant). runs: list to receive (first index, last index, m, d)"""
    n = len(pts); eps = EPS * K; out = pts.copy(); i = 0
    used = np.zeros(n, bool)
    while i < n:
        j = i + 2
        while j < i + n and fit_line(pts[np.arange(i, j + 1) % n])[2] < eps: j += 3
        while j > i + 2 and fit_line(pts[np.arange(i, j + 1) % n])[2] >= eps: j -= 1
        # grown into the next round: shorten from the far end until the run is straight
        while j - i > 4 and np.linalg.norm(pts[(j - 1) % n] - pts[i]) >= RUN_MIN * K and bowed(pts[np.arange(i, j) % n]):
            j -= 2
        idx = np.arange(i, j) % n
        length = np.linalg.norm(pts[idx[-1]] - pts[idx[0]])
        if length >= RUN_MIN * K and not used[idx].any() and not bowed(pts[idx]) and not chord(pts, idx) and \
                (length >= RUN_LONG * K or corner_end(pts, idx) or slanted(pts, idx)):
            m, d, _ = fit_line(pts[idx])
            # the run must not reach into the neighbouring curves: trim TRIM px off both ends
            along = (pts[idx] - m) @ d
            lo, hi = along.min() + TRIM * K, along.max() - TRIM * K
            idx = idx[(along >= lo) & (along <= hi)] if (along.max() - along.min()) > 2 * TRIM * K else idx
            ang = math.atan2(d[0], d[1]) if d[1] else math.pi / 2   # angle from vertical
            # italic stems in image coordinates (y down): going down, they move left
            for target in (-SLANT, -SLANT - math.pi, -SLANT + math.pi):
                if abs(ang - target) < STEM_TOL:
                    d = np.array((-math.sin(SLANT), math.cos(SLANT))); break
            else:
                if abs(abs(ang) - math.pi / 2) < STEM_TOL:   # nearly horizontal: exactly horizontal
                    d = np.array((1.0, 0.0))
            out[idx] = m + np.outer((pts[idx] - m) @ d, d)
            used[idx] = True
            if runs is not None: runs.append((idx[0], idx[-1], m, d))
            i = j
        else:
            i += 1
    return out

SIGMA2 = 0.0         # px: rounds the joins of the straightened runs before the second trace
ALPHAMAX2 = 1.25     # potrace: higher = fewer corners, smoother curves

def trace(img, sigma, alphamax=1.0):
    if sigma: img = img.filter(ImageFilter.GaussianBlur(sigma * K))
    return potrace.Bitmap(np.asarray(img, dtype=float) / 255 > 0.5).trace(turdsize=200, alphamax=alphamax, opticurve=True, opttolerance=0.2)

REFINE_ITERS, REFINE_GAIN = 25, 0.5

def refined(cellimg):
    """the upscaled cell, corrected by iterative back-projection: its thresholded shape, box-filtered back to the
    atlas grid, reproduces the atlas coverage (plain bicubic + threshold misses it by ~1.6 %)"""
    a = np.asarray(cellimg, dtype=float) / 255
    def up(x):
        lo, hi = x.min(), x.max(); s = (hi - lo) or 1
        im = Image.fromarray(((x - lo) / s * 65535).astype(np.int32), mode="I")
        return np.asarray(im.resize((46 * K, 42 * K), Image.BICUBIC), dtype=float) / 65535 * s + lo
    F = up(a)
    for _ in range(REFINE_ITERS):
        F = F + REFINE_GAIN * up(a - (F > 0.5).reshape(42, K, 46, K).mean((1, 3)))
    return Image.fromarray((np.clip(F, 0, 1) * 255).astype("uint8"))

def glyph(cellimg, standard_rounds=True):
    big = refined(cellimg) if REFINE_ITERS else cellimg.resize((46 * K, 42 * K), Image.BICUBIC)
    first = trace(big, SIGMA)
    dens = dense(first)
    filled = np.zeros((42 * K, 46 * K), bool)
    for c in dens:
        t = Image.new("1", (46 * K, 42 * K), 0); ImageDraw.Draw(t).polygon([tuple(p) for p in c], fill=1)
        filled ^= np.asarray(t)
    # Pillow's polygon fill also paints the outline (half a pixel too fat): rasterize 4x finer, then box-filter
    R = 4
    m = np.zeros((42 * K * R, 46 * K * R), bool)
    for c in dens:
        runs = []
        s = straighten(c, runs)
        if standard_rounds: s = fillets(s, runs, filled)
        s = s[np.all(np.isfinite(s), axis=1)]
        if len(s) < 3: continue
        t = Image.new("1", (46 * K * R, 42 * K * R), 0)
        ImageDraw.Draw(t).polygon([tuple(p * R) for p in s], fill=1)
        m ^= np.asarray(t)
    m = m.reshape(42 * K, R, 46 * K, R).mean((1, 3)) > 0.5
    return trace(Image.fromarray((m * 255).astype("uint8")), SIGMA2, ALPHAMAX2)

def render(path, scale):
    size = (46 * scale, 42 * scale); m = np.zeros((size[1], size[0]), bool)
    for c in dense(path, 1.0):
        t = Image.new("1", size, 0); ImageDraw.Draw(t).polygon([tuple(p * scale / K) for p in c], fill=1); m ^= np.asarray(t)
    return Image.fromarray(np.where(m, 0, 255).astype("uint8"))

if __name__ == "__main__":
    cell = load_atlas(sys.argv[1]); chars = sys.argv[3]; sc = 14
    row = Image.new("L", (len(chars) * 46 * sc, 42 * sc), 255)
    for i, ch in enumerate(chars): row.paste(render(glyph(cell(ch)), sc), (i * 46 * sc, 0))
    row.resize((row.width // 2, row.height // 2), Image.LANCZOS).save(sys.argv[2])


def corners(pts, runs, filled, max_gap=6.0):
    """corners between consecutive straight runs: (kind, radius px, angle deg, i_end, j_start, X, d1, d2).
    kind 'inner' when the lines' intersection lies in the glyph (the round adds material), else 'outer'"""
    n = len(pts); out = []
    rs = sorted(runs, key=lambda r: r[0])
    for k in range(len(rs)):
        a, b = rs[k], rs[(k + 1) % len(rs)]
        gap = (b[0] - a[1]) % n
        if gap < 2 or np.linalg.norm(pts[b[0]] - pts[a[1]]) > max_gap * K: continue
        d1 = a[3] * np.sign((pts[a[1]] - pts[a[0]]) @ a[3] or 1)
        d2 = b[3] * np.sign((pts[b[1]] - pts[b[0]]) @ b[3] or 1)
        cr = d1[0] * d2[1] - d1[1] * d2[0]
        if abs(cr) < 0.2: continue                       # nearly parallel: not a corner
        A = np.array([d1, -d2]).T
        t = np.linalg.solve(A, b[2] - a[2])
        X = a[2] + d1 * t[0]
        turn = math.acos(max(-1, min(1, d1 @ d2)))        # direction change
        phi = math.pi - turn                               # angle between the two edges
        gi = np.arange(a[1], a[1] + gap + 1) % n
        dist = np.linalg.norm(pts[gi] - X, axis=1).min()
        r = dist / (1 / math.sin(phi / 2) - 1) / K
        xi, yi = int(round(X[0])), int(round(X[1]))
        inside = 0 <= yi < filled.shape[0] and 0 <= xi < filled.shape[1] and filled[yi, xi]
        out.append(("inner" if inside else "outer", r, math.degrees(phi), a[1], b[0], X, d1, d2))
    return out


R_INNER = 0.8        # px: every inner round (counter corners, stem-bar joins): one standard radius
R_OUTER = 0.6        # px: small outer corners
OUTER_ROUNDS = False # standardize outer corners too (off: they stay as in the atlas)
FILLET_MIN_DEG, FILLET_MAX_DEG = 55, 125   # only corners close to square get the standard rounds
R_OUTER_KEEP = 1.5   # px: outer rounds larger than this are design curves (O, C shoulders): kept as traced

def fillets(pts, runs, filled):
    """replace each corner between two straight runs by an exact arc of the standard radius, tangent to both"""
    n = len(pts); reps = []
    for kind, r, phi_deg, ia, jb, X, d1, d2 in corners(pts, runs, filled):
        if kind == "outer" and (not OUTER_ROUNDS or r > R_OUTER_KEEP): continue
        if not (FILLET_MIN_DEG <= phi_deg <= FILLET_MAX_DEG): continue   # obtuse joins on round shoulders: as traced
        R = (R_INNER if kind == "inner" else R_OUTER) * K
        phi = math.radians(phi_deg)
        L = R / math.tan(phi / 2)
        ra = next(x for x in runs if x[1] == ia); rb = next(x for x in runs if x[0] == jb)
        ia_idx = np.arange(ra[0], ra[0] + (ra[1] - ra[0]) % n + 1) % n
        jb_idx = np.arange(rb[0], rb[0] + (rb[1] - rb[0]) % n + 1) % n
        pa = [i for i in ia_idx if (X - pts[i]) @ d1 >= L]
        qb = [j for j in jb_idx if (pts[j] - X) @ d2 >= L]
        if not pa or not qb: continue
        p, q = pa[-1], qb[0]
        # the replaced stretch must be the corner itself, nothing beyond it (e.g. the foot of [)
        span = np.arange(p, p + (q - p) % n + 1) % n
        if (q - p) % n > n // 3 or np.linalg.norm(pts[span] - X, axis=1).max() > 1.5 * L + R: continue
        T1, T2 = X - d1 * L, X + d2 * L
        bis = (-d1 + d2); bis /= np.linalg.norm(bis)
        C = X + bis * (R / math.sin(phi / 2))
        a1 = math.atan2(*(T1 - C)[::-1]); a2 = math.atan2(*(T2 - C)[::-1])
        da = (a2 - a1 + math.pi) % (2 * math.pi) - math.pi
        arc = [C + R * np.array((math.cos(a1 + da * t), math.sin(a1 + da * t))) for t in np.linspace(0, 1, 12)]
        if not np.all(np.isfinite(arc)) or not np.all(np.isfinite(C)): continue
        reps.append((p, q, arc))
    if not reps: return pts
    skip = {}
    for p, q, arc in reps: skip[p] = (q, arc)
    out, i, start = [], 0, 0
    # walk once around from an index not inside a replaced range
    inside = np.zeros(n, bool)
    for p, q, _ in reps:
        k = p
        while k != q: k = (k + 1) % n; inside[k] = True
    start = int(np.argmin(inside)); i = start; visited = 0
    while visited < n:
        out.append(pts[i])
        if i in skip:
            q, arc = skip[i]
            out += arc
            visited += (q - i) % n; i = q
            continue
        i = (i + 1) % n; visited += 1
    return np.array(out[:-1]) if len(out) > 1 and np.allclose(out[0], out[-1]) else np.array(out)
