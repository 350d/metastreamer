"""Outlines built directly from the traced contour: every straight run is ONE line segment, every corner/curve between
two runs one cubic tangent to both (inner corners: a circular arc of the standard radius), fully curved stretches
fitted with tangent-continuous cubics (Schneider). Coordinates: atlas px * K, y down."""
import math, numpy as np
import straighten as st

TOL = 0.12     # px of the atlas
SMOOTH_PTS = 3 # dense points (2 K-px apart) each side of the moving average along curves: max distance of a fitted cubic from the traced points

def unit(v):
    n = np.linalg.norm(v); return v / n if n else v

def bez(P, u):
    u = u[:, None]; return (1-u)**3*P[0] + 3*(1-u)**2*u*P[1] + 3*(1-u)*u**2*P[2] + u**3*P[3]

def fit_cubic(pts, t1, t2):
    """one cubic from pts[0] to pts[-1], leaving along t1, arriving along -t2 (t2 points back into the curve)"""
    p0, p3 = pts[0], pts[-1]
    d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(pts, axis=0), axis=1))]
    if d[-1] == 0: return [p0, p0, p3, p3], 0.0
    u = d / d[-1]
    for _ in range(4):
        b0, b1, b2, b3 = (1-u)**3, 3*(1-u)**2*u, 3*(1-u)*u**2, u**3
        A1, A2 = np.outer(b1, t1), np.outer(b2, t2)
        C = np.array([[np.sum(A1*A1), np.sum(A1*A2)], [np.sum(A1*A2), np.sum(A2*A2)]])
        X = pts - (np.outer(b0 + b1, p0) + np.outer(b2 + b3, p3))
        rhs = np.array([np.sum(A1*X), np.sum(A2*X)])
        try: a1, a2 = np.linalg.solve(C, rhs)
        except np.linalg.LinAlgError: a1 = a2 = -1
        seg = np.linalg.norm(p3 - p0)
        if a1 < 1e-3 * seg or a2 < 1e-3 * seg: a1 = a2 = seg / 3
        P = [p0, p0 + t1 * a1, p3 + t2 * a2, p3]
        # Newton reparametrization
        q = bez(P, u); dq = 3*((1-u)**2)[:, None]*(P[1]-P[0]) + 6*((1-u)*u)[:, None]*(P[2]-P[1]) + 3*(u**2)[:, None]*(P[3]-P[2])
        ddq = 6*(1-u)[:, None]*(P[2]-2*P[1]+P[0]) + 6*u[:, None]*(P[3]-2*P[2]+P[1])
        num = np.sum((q - pts) * dq, 1); den = np.sum(dq*dq, 1) + np.sum((q - pts)*ddq, 1)
        u = np.clip(u - np.where(np.abs(den) > 1e-9, num / den, 0), 0, 1); u[0], u[-1] = 0, 1
    err = np.linalg.norm(bez(P, u) - pts, axis=1)
    return P, err.max()

def smooth(pts, w=SMOOTH_PTS):
    """moving average along an open stretch, ends kept: the pixel wobble out, the shape in"""
    if len(pts) < 2 * w + 3: return pts
    k = np.ones(2 * w + 1) / (2 * w + 1)
    out = pts.copy()
    for j in (0, 1):
        pad = np.r_[np.full(w, pts[0, j]), pts[:, j], np.full(w, pts[-1, j])]
        out[:, j] = np.convolve(pad, k, mode="valid")
    out[0], out[-1] = pts[0], pts[-1]
    return out

def tangent_at(pts, i, w=10):
    return unit(pts[min(i + w, len(pts) - 1)] - pts[max(i - w, 0)])

def fit_curve(pts, t1, t2, tol, depth=0):
    P, e = fit_cubic(pts, t1, t2)
    if e <= tol or len(pts) < 8 or depth > 8: return [P]
    q = bez(P, np.linspace(0, 1, len(pts)))
    i = int(np.clip(np.argmax(np.linalg.norm(q - pts, axis=1)), 3, len(pts) - 4))
    tm = tangent_at(pts, i)
    return fit_curve(pts[:i + 1], t1, -tm, tol, depth + 1) + fit_curve(pts[i:], tm, t2, tol, depth + 1)

CORNER_MAX = 3.0      # px: a stretch between two straight runs shorter than this is a corner: one exact arc
MERGE_EPS = 1.5       # neighbouring runs that fit one line within MERGE_EPS x the straightness tolerance are one line
ACUTE_DEG = 50        # corners sharper than this always get the best arc
ARC_TOL = 0.35        # px: a stretch further than this from the arc is a curve, not a corner
DEBUG = False
MIN_LINE = 1.0       # px
SHALLOW_DEG, SHALLOW_FRAC = 20, 0.5   # a join turning less: blended over this share of the shorter line
ARC_LOOP_TOL = 0.45   # px: ... but a fitted curve that loops at a corner gives way to an arc this close
CORNER_NEAR = 1.5     # px: the contour passing this close to the intersection of two lines = a corner, however long
R_OUTER_MIN, R_OUTER_MAX = 0.25, 2.5

def loops(fit, d0):
    """the fitted pieces turn back against their overall turn (a hook or loop at a tight corner)"""
    q = np.vstack([bez(np.array(P), np.linspace(0, 1, 40)) for P in fit])
    v = np.diff(q, axis=0); v = v[np.linalg.norm(v, axis=1) > 1e-9]
    if len(v) < 3: return False
    cr = v[:-1, 0] * v[1:, 1] - v[:-1, 1] * v[1:, 0]
    ang = np.arctan2(cr, np.sum(v[:-1] * v[1:], axis=1))
    tot = ang.sum()
    return np.sum(np.clip(-np.sign(tot) * ang, 0, None)) > math.radians(10)

def contour_segments(pts, filled):
    """[('line', a, b) | ('cubic', p0, p1, p2, p3)] for one closed dense contour"""
    n = len(pts); K = st.K; tol = TOL * K
    runs = []
    st.straighten(pts, runs)
    ring = np.vstack([pts[-6:], pts, pts[:6]])
    if not runs:   # fully curved: split into 4, tangent-continuous at the splits
        ring_s = smooth(np.vstack([pts[-SMOOTH_PTS:], pts, pts[:SMOOTH_PTS]]))[SMOOTH_PTS:-SMOOTH_PTS]
        pts = ring_s; ring = np.vstack([pts[-6:], pts, pts[:6]])
        idx = [0, n // 4, n // 2, 3 * n // 4, n]; ext = np.vstack([pts, pts[:1]]); out = []
        for a, b in zip(idx, idx[1:]):
            out += [("cubic", *P) for P in fit_curve(ext[a:b + 1], tangent_at(ring, a + 6), -tangent_at(ring, b + 6), tol)]
        return out
    runs.sort(key=lambda r: r[0])
    # lines: [start, end, direction, first index, last index]
    lines = []
    for i0, i1, m, d in runs:
        d = d * np.sign((pts[i1] - pts[i0]) @ d or 1)
        lines.append([i0, i1, d, m])
    # merge neighbouring collinear runs
    merged = True
    while merged and len(lines) > 1:
        merged = False
        for k in range(len(lines)):
            A, B = lines[k], lines[(k + 1) % len(lines)]
            if A is B: break
            idx = np.arange(A[0], A[0] + (B[1] - A[0]) % n + 1) % n
            if len(idx) > n * 0.9 or A[2] @ B[2] < 0.9: continue
            m, d, res = st.fit_line(pts[idx])
            # one line if both runs and the stretch between them fit it as well as a single run would
            if res < MERGE_EPS * st.EPS * K and not st.bowed(pts[idx]):
                d = d * np.sign(A[2] @ d or 1)
                lines[k] = [A[0], B[1], d, m]; del lines[(k + 1) % len(lines) if (k + 1) % len(lines) > k else 0]
                merged = True; break
    L = []
    for i0, i1, d, m in lines:
        L.append({"a": m + d * ((pts[i0] - m) @ d), "b": m + d * ((pts[i1] - m) @ d), "d": d, "i0": i0, "i1": i1})
    # a stub (a run trimmed to almost nothing, e.g. in the tip of a notch) would break the corner of its neighbours
    # ... when its neighbours meet at an angle close to it (a step between parallel edges stays)
    k = 0
    while len(L) > 2 and k < len(L):
        P, C, N = L[k - 1], L[k], L[(k + 1) % len(L)]
        if np.linalg.norm(C["b"] - C["a"]) < MIN_LINE * K:
            d1, d2 = P["d"], N["d"]
            if d1 @ d2 < 0.9 and abs(d1[0] * d2[1] - d1[1] * d2[0]) > 0.05:
                X = P["a"] + d1 * np.linalg.solve(np.array([d1, -d2]).T, N["b"] - P["a"])[0]
                if np.linalg.norm(X - (C["a"] + C["b"]) / 2) < 2 * MIN_LINE * K:
                    if DEBUG: print("stub dropped")
                    del L[k]; continue
        k += 1
    joins = []; spare = {}
    for k in range(len(L)):
        A, B = L[k], L[(k + 1) % len(L)]
        gap = np.arange(A["i1"], A["i1"] + (B["i0"] - A["i1"]) % n + 1) % n
        glen = np.sum(np.linalg.norm(np.diff(pts[gap], axis=0), axis=1)) if len(gap) > 1 else 0
        d1, d2 = A["d"], B["d"]
        cr = d1[0] * d2[1] - d1[1] * d2[0]
        joins.append(None)
        if abs(cr) < 0.05: continue
        X = A["a"] + d1 * np.linalg.solve(np.array([d1, -d2]).T, B["b"] - A["a"])[0]
        near = np.linalg.norm(pts[gap] - X, axis=1).min() if len(gap) else 0
        if glen > CORNER_MAX * K and near > CORNER_NEAR * K: continue
        phi = math.pi - math.acos(max(-1, min(1, d1 @ d2)))       # angle between the edges
        xi, yi = int(round(X[0])), int(round(X[1]))
        inside = 0 <= yi < filled.shape[0] and 0 <= xi < filled.shape[1] and filled[yi, xi]
        room = min((X - A["a"]) @ d1, (B["b"] - X) @ d2) * 0.95
        if room <= 0: continue
        turn = math.pi - phi
        def arc_dev(R):
            """the arc of radius R tangent to both lines (shrunk to fit on them): its points and the largest
            distance of the traced stretch from lines + arc"""
            Lt = R / math.tan(phi / 2)
            if Lt > room: Lt = room; R = Lt * math.tan(phi / 2)
            h = 4 / 3 * math.tan(turn / 4) * R
            T1, T2 = X - d1 * Lt, X + d2 * Lt
            path = np.vstack([A["a"] + np.outer(np.linspace(0, 1, 60), T1 - A["a"]),
                              bez(np.array([T1, T1 + d1 * h, T2 - d2 * h, T2]), np.linspace(0, 1, 80)),
                              T2 + np.outer(np.linspace(0, 1, 60), B["b"] - T2)])
            dev = np.min(np.linalg.norm(pts[gap][:, None, :] - path[None, :, :], axis=2), axis=1).max() if len(gap) else 0
            return dev, Lt, R
        best = None
        if turn < math.radians(SHALLOW_DEG):   # two chords of a flat arch (the top of C): blend them over a long stretch
            Lt = SHALLOW_FRAC * min(np.linalg.norm(A["b"] - A["a"]), np.linalg.norm(B["b"] - B["a"]))
            while Lt > 0.2 * K:
                dev, Lt2, R = arc_dev(Lt * math.tan(phi / 2))
                if dev <= ARC_TOL * K: best = (Lt2, R); break
                Lt *= 0.7
        if best is None:      # the radius that follows the atlas best
            cands = [arc_dev(r * K) for r in np.geomspace(R_OUTER_MIN, R_OUTER_MAX, 18)]
            dev, Lt, R = min(cands, key=lambda c: c[0])
            if dev <= ARC_TOL * K: best = (Lt, R)
            elif phi < math.radians(ACUTE_DEG): best = (Lt, R)   # acute tips: a fitted curve would loop
        if best is None:                          # a real curve: fitted below (the arc if the fit loops)
            spare[k] = (X, Lt, R, turn, dev); continue
        joins[-1] = (X, best[0], best[1], turn)
    for k, J in enumerate(joins):
        if J is None: continue
        X, Lt, R, turn = J; A, B = L[k], L[(k + 1) % len(L)]
        A["b"] = X - A["d"] * Lt; B["a"] = X + B["d"] * Lt
    segs = []
    for k in range(len(L)):
        A, B = L[k], L[(k + 1) % len(L)]
        segs.append(("line", A["a"].copy(), A["b"].copy()))
        J = joins[k]
        if J is not None:
            X, Lt, R, turn = J; h = 4 / 3 * math.tan(turn / 4) * R
            segs.append(("cubic", A["b"], A["b"] + A["d"] * h, B["a"] - B["d"] * h, B["a"]))
        else:
            gap = np.arange(A["i1"], A["i1"] + (B["i0"] - A["i1"]) % n + 1) % n
            pg = smooth(np.vstack([A["b"], pts[gap[1:-1]] if len(gap) > 2 else np.empty((0, 2)), B["a"]]))
            if np.linalg.norm(B["a"] - A["b"]) < 1e-6: continue
            fit = fit_curve(pg, A["d"], -B["d"], tol)
            if k in spare and loops(fit, A["d"]) and spare[k][4] <= ARC_LOOP_TOL * K:
                X, Lt, R, turn, _ = spare[k]
                if DEBUG: print("arc instead of loop: dev %.2f px" % (spare[k][4] / K))
                if True:
                    T1, T2 = X - A["d"] * Lt, X + B["d"] * Lt; h = 4 / 3 * math.tan(turn / 4) * R
                    segs[-1] = ("line", A["a"].copy(), T1)
                    segs.append(("cubic", T1, T1 + A["d"] * h, T2 - B["d"] * h, T2))
                    if np.linalg.norm(B["a"] - T2) > 1e-6: segs.append(("line", T2, B["a"].copy()))
                    continue
            segs += [("cubic", *P) for P in fit]
    return segs

# ! and ?: the atlas barely separates the dot (a half-transparent row); cut a clear gap above it
DOT_TOP = 34.0 - 128 / 25.6      # atlas row of the top of the period
DOT_GAP = 2.5                    # px
DOTTED = "!?"
SLIVER = 0.55                    # px
CLOSE = {}                         # px: radius of the closing, per glyph (the hairlines in the strokes of #)

def glyph_contours(cellimg, ch=None):
    """contours of one atlas cell as segment lists, holes included"""
    K = st.K
    big = st.refined(cellimg) if st.REFINE_ITERS else cellimg.resize((46 * K, 42 * K))
    if ch and ch in DOTTED:
        a = np.asarray(big.convert("L")).copy()
        a[int(round((DOT_TOP - DOT_GAP) * K)):int(round(DOT_TOP * K)), :] = 0
        from PIL import Image as _I
        big = _I.fromarray(a)
    # potracer traces the False pixels: give it the inverted mask, it returns the glyph's own contours.
    # Morphological closing first: hairline gaps (the strokes of # drawn with a hairline between them) are filled
    from PIL import ImageOps, ImageFilter
    mask = big.convert("L").point(lambda v: 255 if v > 127 else 0)
    k = 2 * int(round(CLOSE.get(ch, 0) * K)) + 1
    if k > 1: mask = mask.filter(ImageFilter.MaxFilter(k)).filter(ImageFilter.MinFilter(k))
    first = st.trace(ImageOps.invert(mask), 0)
    dens = st.dense(first)
    # hairline holes are rasterization artifacts of the atlas (the strokes of # drawn with a hairline gap):
    # a contour whose mean width (2 area / perimeter) is under SLIVER px is dropped
    def width(c):
        a = abs(0.5 * np.sum(c[:, 0] * np.roll(c[:, 1], -1) - np.roll(c[:, 0], -1) * c[:, 1]))
        per = np.sum(np.linalg.norm(c - np.roll(c, -1, 0), axis=1))
        return 2 * a / per / K
    dens = [c for c in dens if width(c) >= SLIVER]
    W, H = 46 * K, 42 * K
    from PIL import Image, ImageDraw
    filled = np.zeros((H, W), bool)
    for c in dens:
        t = Image.new("1", (W, H), 0); ImageDraw.Draw(t).polygon([tuple(p) for p in c], fill=1); filled ^= np.asarray(t)
    return [contour_segments(c, filled) for c in dens], dens
