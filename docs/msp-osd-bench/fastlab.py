#!/usr/bin/env python3
"""fastlab: measure how much OSD the DJI goggles accept in one frame, over MSP DisplayPort (O3 Air Unit + Goggles 2).

The goggles silently throw away a whole frame (CLEAR ... DRAW) when it carries too much, and keep showing the previous
one. A trial therefore draws a marker label ("PREVIOUS FRAME", row 19 col 38; the test frames never touch row 19), waits
until the label is visible on the goggles' video snapshot, sends the test frame and polls the snapshot: label gone =
"shown", label still there after 1.9 s = "dropped". Test frames from byte streams (--streams, .bin files) may cover the
label cell, so they are judged by pixel difference against the reference screen instead (a different screen at all =
shown). The limit is found by bisection: bisect() shrinks [lo, hi) until lo is the largest N still shown; the frames at
N and N+1 are saved as <out>/<prefix><name>-<N>-ok.jpg / -drop.jpg. All writes are single-glyph WRITEs unless a group says
otherwise, so N counts cells; geometry (solid rows, isolated cells, runs) and glyph code are the variables.

Needs: Python 3, pyserial, Pillow, msplab.py (this folder: the fake flight controller that keeps the air unit happy), and
a snapshot URL that returns the goggles' image as JPEG (e.g. metastreamer: http://metastreamer/snapshot). Set the
goggles menu first: Flight Controller = Betaflight (or INAV with --mode inav) and Canvas Mode = HD (53x20).

  fastlab.py core                       the headline numbers: solid rows of 53 from col 0 -> 637, rows of 50 from col 2 -> 620,
                                        isolated X every other column (26 per row) -> 361, XX pairs with one empty cell,
                                        17 per row -> 459 (Betaflight mode); INAV mode gives 537 / 526 / 326 / 407
  fastlab.py G1                         every glyph code 0x00-0xFF: 630 single WRITEs in solid rows of 53 (630 < 637, the limit for
                                        X in that geometry): a dropped code costs more than X. Controls (X x630 shown, X x638
                                        dropped) at start, every 64 codes and at the end show the detector did not drift.
                                        INAV mode: --limit 537 --n 520; --first/--last resume a pass, --codes picks codes
  fastlab.py E1 G C1 C2 C3 C4 D1 D2 D3  glyph cost (0x58, 0x30, 0x01, 0x06, 0x70, 0x7E, 0x90, 0x9C), row geometry (width and start
                                        column), isolated cells with step 2-6, write order (forward, reversed, random),
                                        glyph entropy, 0x01/X mixes, run length and gap width: all bisections of N
  fastlab.py cheap                      every code at 700 WRITEs (--n), solid rows of 53: which codes cost less than X; bisect those
  fastlab.py mix checker release nul    0x01/X arrangements; stripes vs checkerboard; does RELEASE free the budget; NUL in strings
  fastlab.py R                          R1 solid 400 block, R4 prefix bisection of <streams>/single-B1.bin
  fastlab.py V                          the byte streams v3-frame{A0,A1,B0,B1,C1}-pr.bin and the multi-frame streams, after prev.bin
  fastlab.py I                          single real frames (single-A1/A0/B1.bin, B1 prefixes 118/119) and attr pages (see attr)
  fastlab.py attr                       "LOW BATTERY 3.3V" with font-page attr 0,1,2,3 on rows 4,6,8,10, and attr 0 everywhere
  fastlab.py --mode inav --prefix inav- core I attr    the INAV-mode set

Byte streams: <streams>/*.bin are raw MSP v1 frames (command 182 only, as the Betaflight driver emits them); prev.bin draws
"PREVIOUS FRAME" at row 9 col 19; README.txt there says what each file contains and what is expected. Frames are sent one
MSP frame at a time with 2 ms gaps. Results go to <out>/results-<mode>.txt, one line per measurement.
"""
import argparse, io, os, random, sys, threading, time, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from msplab import FC  # noqa: E402

LABEL_ROW, LABEL_COL, LABEL = 19, 38, 'PREVIOUS FRAME'
COLS, ROWS = 53, 20
DROP_WAIT = 1.9
# rows the goggles' own widgets use (fractions of the image): bottom right box, top right clock; ignored by the pixel diff
IGNORE = ((0.65, 0.77, 1.0, 1.0), (0.80, 0.26, 1.0, 0.37))


class Lab:
    def __init__(self, a):
        import serial
        self.a = a
        self.fc = FC(serial.Serial(a.port, a.baud, timeout=0.05))
        threading.Thread(target=self.fc.rx_loop, daemon=True).start()
        threading.Thread(target=self.fc.heartbeat_loop, daemon=True).start()
        os.makedirs(a.out, exist_ok=True)
        for _ in range(9):   # the goggles ignore DisplayPort for the first seconds of a new session
            self.fc.clear(); self.fc.text(0, 0, 'FASTLAB'); self.fc.draw(); time.sleep(1)

    # ---- snapshots ----
    def raw_snap(self):
        for _ in range(6):   # the bridge answers 504 now and then while the goggles re-sync
            try:
                return urllib.request.urlopen(self.a.snapshot, timeout=6).read()
            except Exception:
                time.sleep(0.5)
        raise RuntimeError('snapshot unavailable: ' + self.a.snapshot)

    def snap(self):
        from PIL import Image
        return Image.open(io.BytesIO(self.raw_snap())).convert('L')

    @staticmethod
    def lit(im, row, col):
        W, H = im.size
        cw, ch = W / COLS, H / ROWS
        x0, y0 = int(col * cw) + 3, int(row * ch) + 5
        pts = [(x, y) for x in range(x0, min(W - 1, int((col + 1) * cw) - 3), 3)
               for y in range(y0, min(H - 1, int((row + 1) * ch) - 5), 3)]
        return max(im.getpixel(p) for p in pts) > 110

    def save(self, name):
        open(os.path.join(self.a.out, self.a.prefix + name), 'wb').write(self.raw_snap())

    # ---- trials ----
    def prev(self):
        self.fc.clear(); self.fc.text(LABEL_ROW, LABEL_COL, LABEL); self.fc.draw()
        t0 = time.time()
        while time.time() - t0 < 4:
            if self.lit(self.snap(), LABEL_ROW, LABEL_COL): return

    def trial(self, writes, img=None):
        """writes: [(row, col, bytes)] drawn as one frame after CLEAR; True if the goggles showed it"""
        self.prev(); fc = self.fc; fc.clear()
        for w in writes: fc.text(w[0], w[1], w[2], attr=w[3] if len(w) > 3 else 0, limit=32)
        fc.draw(); t0 = time.time(); ok = False
        while time.time() - t0 < DROP_WAIT:
            if not self.lit(self.snap(), LABEL_ROW, LABEL_COL): ok = True; break
        if img:
            if ok: time.sleep(0.3)
            self.save(img)
        return ok

    def shown(self, writes):
        """a frame counts as dropped only if two attempts in a row are: a single lost frame must not move the bisection"""
        return self.trial(writes) or self.trial(writes)

    def bisect(self, make, lo, hi):
        """make(n) -> writes. Largest n in [lo, hi) shown, or a text if lo is already dropped / hi still shown."""
        if not self.shown(make(lo)): return f'even {lo} dropped'
        if self.trial(make(hi)): return f'{hi} shown, no limit found up to {hi}'
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if self.shown(make(mid)): lo = mid
            else: hi = mid
        return lo

    def measure(self, group, name, make, lo, hi, img=None):
        print(f'[{group}] ... {name}', flush=True)
        t = time.time(); res = self.bisect(make, lo, hi)
        if img and isinstance(res, int):
            self.trial(make(res), f'{img}-{res}-ok.jpg'); self.trial(make(res + 1), f'{img}-{res + 1}-drop.jpg')
        self.log(group, f'{name}: Nmax = {res}' if isinstance(res, int) else f'{name}: {res}', time.time() - t)
        return res

    def log(self, group, line, secs=None):
        print(f'[{group}] {line}' + (f'   ({secs:.0f} s)' if secs else ''), flush=True)
        with open(os.path.join(self.a.out, f'results-{self.a.mode}.txt'), 'a') as f: f.write(f'[{group}] {line}\n')

    # ---- byte streams ----
    def frames(self, fn):
        b = open(os.path.join(self.a.streams, fn), 'rb').read(); i = 0; out = []
        while i < len(b): ln = b[i + 3]; out.append(b[i:i + 6 + ln]); i += 6 + ln
        return out

    def real_trial(self, send, img=None):
        """prev.bin, then send(): shown if the screen changed at all (the label cell may be covered by the frame)"""
        from PIL import ImageChops
        for f in self.frames('prev.bin'): self.fc.write(f)
        t0 = time.time()
        while time.time() - t0 < 4:
            if self.lit(self.snap(), 9, 19): break
        base = self.snap(); send(); t0 = time.time(); changed = 0
        while time.time() - t0 < 2.4:
            im = self.snap(); d = ImageChops.difference(im, base).point(lambda v: 255 if v > 80 else 0)
            W, H = d.size
            for x0, y0, x1, y1 in IGNORE: d.paste(0, (int(x0 * W), int(y0 * H), int(x1 * W), int(y1 * H)))
            changed = sum(1 for v in d.tobytes() if v)
            if changed > 3000: break
        if img:
            if changed > 3000: time.sleep(0.3)
            self.save(img)
        return changed > 3000, changed

    def send_stream(self, fn):
        def go():
            for f in self.frames(fn): self.fc.write(f); time.sleep(0.002)
        return go


# ---- cell layouts (row, col) in screen order; rows 0-18, row 19 belongs to the label ----
def solid(w, c0): return [(k // w, c0 + k % w) for k in range(19 * w)]
def isolated(step): return [(r, c) for r in range(19) for c in range(0, 53, step)]
def isolated26(): return [(r, 2 * c) for r in range(19) for c in range(26)]
def pairs(per_row, gap): return [(r, (2 + gap) * k) for r in range(19) for k in range(per_row)]

def singles(cells, glyph=b'X'):
    return lambda n: [(r, c, glyph) for r, c in cells[:n]]

def singles_by(cells, f):
    return lambda n: [(r, c, f(k, r, c)) for k, (r, c) in enumerate(cells[:n])]

def runs(L, gap):
    """runs of L cells separated by gap empty cells, one WRITE each, rows 0-18 from col 0; N counts cells"""
    plan = []
    for r in range(19):
        c = 0
        while c + L <= 53: plan.append((r, c, L)); c += L + gap
    def make(n):
        w = []; left = n
        for r, c, l in plan:
            if left <= 0: break
            k = min(l, left); w.append((r, c, b'X' * k)); left -= k
        return w
    return make, sum(l for _, _, l in plan)


# ---- groups ----
def g_core(lab):
    lab.measure('core', 'solid rows of 53 from col 0', singles(solid(53, 0)), 300, 1007, 'solid53')
    lab.measure('core', 'solid rows of 50 from col 2', singles(solid(50, 2)), 300, 951, 'solid50')
    cells = isolated26()
    lab.measure('core', 'isolated X every other column, 26 per row', singles(cells), 100, len(cells), 'isolated')
    pc = pairs(17, 1)
    lab.measure('core', 'XX pairs, 1 empty cell between, 17 per row (N in cells)', lambda n: [(r, c, b'XX') for r, c in pc[:n // 2]], 100, 2 * len(pc), 'pairs')

def g_E1(lab):
    for code in (0x58, 0x30, 0x01, 0x06, 0x70, 0x7E, 0x90, 0x9C):
        lab.measure('E1', f'glyph 0x{code:02X}, solid rows of 50 from col 2', singles(solid(50, 2), bytes([code])), 280, 951)

def g_G(lab):
    for w, c0 in ((53, 0), (52, 0), (51, 1), (50, 0), (50, 2), (50, 3), (40, 0), (40, 13)):
        lab.measure('G', f'X solid rows of {w} from col {c0}', singles(solid(w, c0)), 300, min(1007, 19 * w + 1))

def g_C1(lab):
    for s in (2, 3, 4, 5, 6):
        cells = isolated(s)
        lab.measure('C1', f'isolated X every {s} cols (rows 0-18 from col 0, {len(cells)} cells max)', singles(cells), 60, len(cells))

def g_C2(lab):
    cells = solid(50, 2)
    lab.measure('C2', 'X rows of 50 from col 2, in order', singles(cells), 400, 900)
    lab.measure('C2', 'same cells reversed', lambda n: [(r, c, b'X') for r, c in reversed(cells[:n])], 400, 900)
    def shuffled(n):
        sel = cells[:n]; order = random.Random(n).sample(range(len(sel)), len(sel))
        return [(sel[i][0], sel[i][1], b'X') for i in order]
    lab.measure('C2', 'same cells in random order', shuffled, 400, 900)

def g_C3(lab):
    cells = solid(53, 0); letters = [bytes([65 + i]) for i in range(16)]; rnd = random.Random(7)
    lab.measure('C3', 'solid rows of 53: all X', singles(cells), 500, 1007)
    lab.measure('C3', 'alternating X/Y', singles_by(cells, lambda k, r, c: b'XY'[k % 2:k % 2 + 1]), 500, 1007)
    lab.measure('C3', 'cycle of 16 letters', singles_by(cells, lambda k, r, c: letters[k % 16]), 500, 1007)
    lab.measure('C3', 'random A-Z', singles_by(cells, lambda k, r, c: bytes([65 + rnd.randrange(26)])), 500, 1007)

def g_C4(lab):
    cells = solid(53, 0)
    lab.measure('C4', 'alternating 0x01 / X', singles_by(cells, lambda k, r, c: b'\x01' if k % 2 else b'X'), 500, 1007)
    lab.measure('C4', 'first half rows 0x01, rest X', singles_by(cells, lambda k, r, c: b'\x01' if r < 9 else b'X'), 500, 1007)

def g_D1(lab):
    for L in (1, 2, 3, 5, 10, 25):
        mk, mx = runs(L, 1); lab.measure('D1', f'X runs of {L} with 1 empty cell between ({mx} cells max)', mk, 300, mx)

def g_D2(lab):
    for g in (3, 10):
        mk, mx = runs(2, g); lab.measure('D2', f'XX pairs with {g}-cell gap ({mx} cells max)', mk, 200, mx)

def g_D3(lab):
    cells = isolated(2)
    lab.measure('D3', 'isolated 0x01 every other column', singles(cells, b'\x01'), 200, len(cells))
    lab.measure('D3', 'isolated X every other column (control)', singles(cells), 200, len(cells))

def g_G1(lab):
    a = lab.a; cells = [(k // 53, k % 53) for k in range(1007)]
    mk = lambda code: singles(cells, bytes([code]))(a.n)
    def control(tag):
        ok1 = lab.trial(singles(cells)(a.n)); ok2 = lab.trial(singles(cells)(a.limit + 1))
        lab.log('G1', f'control {tag}: X x{a.n} {"shown" if ok1 else "DROPPED"} (expect shown), X x{a.limit + 1} {"shown!" if ok2 else "dropped"} (expect dropped)')
    control('start'); dropped = []
    codes = [int(x, 0) for x in a.codes.split(',')] if a.codes else list(range(a.first, a.last + 1))
    for code in codes:
        if not (lab.trial(mk(code)) or lab.trial(mk(code))):   # one retry: a lone drop may be a lost frame
            dropped.append(code); lab.log('G1', f'0x{code:02X} DROPPED at {a.n}')
        if code % 64 == 63: control(f'after 0x{code:02X}')
    control('end')
    lab.log('G1', f'dropped at {a.n} (cost more than X): ' + (' '.join(f'0x{c:02X}' for c in dropped) or 'none'))

def g_cheap(lab):
    """every glyph code at --n single WRITEs (default 700) in solid rows of 53, above the X limit (637): a code that is
    shown costs less than X. Then the shown codes are bisected up to 1007 (the whole test area, rows 0-18)."""
    a = lab.a; n = a.n if a.n != 630 else 700; cells = solid(53, 0)
    mk = lambda code: singles(cells, bytes([code]))
    ok1 = lab.trial(mk(0x58)(637)); ok2 = lab.trial(mk(0x58)(n))
    lab.log('cheap', f'control: X x637 {"shown" if ok1 else "DROPPED"} (expect shown), X x{n} {"SHOWN" if ok2 else "dropped"} (expect dropped)')
    codes = [int(x, 0) for x in a.codes.split(',')] if a.codes else list(range(a.first, a.last + 1))
    cheap = []
    for code in codes:
        if lab.trial(mk(code)(n)): cheap.append(code); lab.log('cheap', f'0x{code:02X} shown at {n}')
    lab.log('cheap', f'shown at {n} (cost less than X): ' + (' '.join(f'0x{c:02X}' for c in cheap) or 'none'))
    for code in cheap:
        lab.measure('cheap', f'0x{code:02X} solid rows of 53', mk(code), n, len(cells))

def g_mix(lab):
    """0x01 mixed with X in solid rows of 53: which arrangement gives the 0x01 discount (alternating = 850, half/half = 757)"""
    cells = solid(53, 0); one, X = b'\x01', b'X'
    for name, f in (('0x01 every 4th cell (k%4==0), X else', lambda k, r, c: one if k % 4 == 0 else X),
                    ('0x01 3 of 4 cells (k%4!=0), X else', lambda k, r, c: one if k % 4 else X),
                    ('0x01 on even columns, X on odd (vertical stripes)', lambda k, r, c: one if c % 2 == 0 else X),
                    ('0x01 on even rows, X on odd rows', lambda k, r, c: one if r % 2 == 0 else X),
                    ('0x01 in cols 0-26, X in cols 27-52', lambda k, r, c: one if c < 27 else X),
                    ('alternating 0x01 / X (k%2, repeat of C4)', lambda k, r, c: one if k % 2 else X),
                    ('alternating X / 0x01 (k%2, other phase)', lambda k, r, c: X if k % 2 else one),
                    ('alternating 0x10 / X (k%2, both >= 0x10)', lambda k, r, c: b'\x10' if k % 2 else X),
                    ('alternating 0x0F / 0x10 (k%2)', lambda k, r, c: b'\x0f' if k % 2 else b'\x10'),
                    ('alternating 0xA0 / X (k%2)', lambda k, r, c: b'\xa0' if k % 2 else X),
                    ('alternating 0x06 / X (k%2)', lambda k, r, c: b'\x06' if k % 2 else X)):
        lab.measure('mix', name, singles_by(cells, f), 500, len(cells))

def g_checker(lab):
    """isolated X, 26 per row: same columns in every row (stripes) against shifted by one on odd rows (checkerboard)"""
    stripes = [(r, 2 * c) for r in range(19) for c in range(26)]
    checker = [(r, 2 * c + (r % 2)) for r in range(19) for c in range(26)]
    lab.measure('checker', 'stripes: X at cols 0,2..50 in every row', singles(stripes), 200, len(stripes))
    lab.measure('checker', 'checkerboard: X at cols 0,2..50 on even rows, 1,3..51 on odd rows', singles(checker), 200, len(checker))

def g_release(lab):
    """does RELEASE (sub-command 1) free the budget like CLEAR? 500 X, DRAW, then RELEASE / nothing / CLEAR,
    then 300 more X elsewhere and a marker, DRAW: 800 cells since the last CLEAR is over the limit"""
    fc = lab.fc
    for how in ('nothing', 'RELEASE', 'CLEAR', 'RELEASE'):
        fc.clear(); fc.draw(); time.sleep(1)
        for r in range(10): fc.text(r, 2, b'X' * 50)
        fc.draw(); time.sleep(1.5)
        if how == 'RELEASE': fc.dp(1)
        if how == 'CLEAR': fc.clear()
        for r in range(10, 16): fc.text(r, 2, b'X' * 50)
        fc.text(17, 2, 'MARKER ' + how); fc.draw(); t0 = time.time(); ok = False
        while time.time() - t0 < 2.5:
            if lab.lit(lab.snap(), 17, 2): ok = True; break
        time.sleep(0.3); lab.save(f'release-{how}.jpg')
        lab.log('release', f'500 X, DRAW, {how}, +300 X and marker, DRAW: {"shown" if ok else "dropped"}')

def g_nul(lab):
    """NUL bytes inside a WRITE string: terminator, skipped, or an empty cell?"""
    fc = lab.fc; fc.clear()
    fc.text(4, 2, b'AB\x00CD'); fc.text(6, 2, b'\x00ABCD'); fc.text(8, 2, b'ABCD\x00'); fc.text(10, 2, b'EFGH')
    fc.text(10, 2, b'\x00\x00'); fc.text(12, 2, 'IJKL'); fc.text(12, 3, b'\x00')
    fc.draw(); time.sleep(1.5); lab.save('nul.jpg')
    im = lab.snap(); row = lambda r: ''.join('#' if lab.lit(im, r, c) else '.' for c in range(2, 9))
    for r, what in ((4, 'AB NUL CD'), (6, 'NUL ABCD'), (8, 'ABCD NUL'), (10, 'EFGH then NUL NUL over it'), (12, 'IJKL then NUL at col 3')):
        lab.log('nul', f'row {r} {what}: cols 2-8 lit {row(r)}')

def g_erase(lab):
    """after a DRAW of 500 X (rows 0-9, cols 2-51), erase rows 0-3 (200 cells) with NUL / with spaces / not at all /
    CLEAR, then add 250 X (rows 10-14) and a marker and DRAW: does erasing with NUL give the budget back?"""
    fc = lab.fc
    def rows(r0, r1, ch):
        for r in range(r0, r1): fc.text(r, 2, ch * 25); fc.text(r, 27, ch * 25)
    for how in ('nothing', 'spaces', 'NUL', 'CLEAR', 'NUL', 'spaces'):
        fc.clear(); fc.draw(); time.sleep(1)
        rows(0, 10, b'X'); fc.draw(); time.sleep(1.5)
        if how == 'spaces': rows(0, 4, b' ')
        if how == 'NUL': rows(0, 4, b'\x00')
        if how == 'CLEAR': fc.clear()
        rows(10, 15, b'X'); fc.text(17, 2, 'MARKER'); fc.draw(); t0 = time.time(); ok = False
        while time.time() - t0 < 2.5:
            if lab.lit(lab.snap(), 17, 2): ok = True; break
        time.sleep(0.3); lab.save(f'erase-{how}.jpg')
        lab.log('erase', f'500 X, DRAW, erase 200 with {how}, +250 X and marker, DRAW: {"shown" if ok else "dropped"}')

def g_align(lab):
    """pairs XX with two empty cells between, by linear index L = row*53 + col: pairs at L = 4m, 4m+1 (each pair fills one
    even-aligned 16-bit word) against L = 4m+1, 4m+2 (each pair straddles two words). Same geometry shifted by one cell."""
    def cells(off): return [divmod(L, 53) for m in range(19 * 53 // 4) for L in (4 * m + off, 4 * m + off + 1) if L < 19 * 53]
    for off, name in ((0, 'aligned: L = 4m, 4m+1'), (1, 'straddling: L = 4m+1, 4m+2'), (2, 'aligned: L = 4m+2, 4m+3'), (3, 'straddling: L = 4m+3, 4m+4')):
        c = cells(off); lab.measure('align', f'XX pairs, {name}', singles(c), 200, len(c))

def g_inavmodel(lab):
    """INAV goggles mode: the same probes that pinned the Betaflight-mode model (align, code classes, page attr)"""
    g_align(lab)
    cells = solid(53, 0)
    for code in (0x01, 0x0F, 0x10, 0x7F, 0x80, 0xFF):
        lab.measure('inav', f'0x{code:02X} solid rows of 53', singles(cells, bytes([code])), 300, len(cells))
    lab.measure('inav', 'X attr 1 (font page 1) solid rows of 53', lambda n: [(r, c, b'X', 1) for r, c in cells[:n]], 300, len(cells))
    lab.measure('inav', 'X attr 2 (font page 2) solid rows of 53', lambda n: [(r, c, b'X', 2) for r, c in cells[:n]], 300, len(cells))
    g_checker(lab)

def g_R(lab):
    cells = [(k // 50, 2 + k % 50) for k in range(400)]
    lab.log('R', 'R1 solid 400 (8 rows x 50): ' + ('shown' if lab.trial([(r, c, b'X') for r, c in cells], 'solid400.jpg') else 'dropped'))
    fr = lab.frames('single-B1.bin')
    clr = [f for f in fr if f[4] == 182 and f[5] == 2][0]; drw = [f for f in fr if f[4] == 182 and f[5] == 4][0]
    wr = [f for f in fr if f[4] == 182 and f[5] == 3]
    def prefix(K):
        def go():
            lab.fc.write(clr)
            for f in wr[:K]: lab.fc.write(f); time.sleep(0.002)
            lab.fc.write(drw)
        return lab.real_trial(go)[0]
    lo, hi = 1, len(wr)
    if prefix(hi): lab.log('R', 'R4 whole single-B1 shown'); return
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if prefix(mid): lo = mid
        else: hi = mid
    lab.log('R', f'R4 single-B1.bin ({len(wr)} WRITEs): prefix K={lo} shown ({sum(len(f) - 10 for f in wr[:lo])} chars), K={hi} dropped')

def g_V(lab):
    for fn, name in (('v3-frameA0-pr.bin', 'v3-A0'), ('v3-frameA1-pr.bin', 'v3-A1'), ('v3-frameB0-pr.bin', 'v3-B0'), ('v3-frameB1-pr.bin', 'v3-B1'),
                     ('v3-frameB-pr-3frames.bin', 'v3-B-stream'), ('v3-frameC1-pr.bin', 'v3-C')):
        ok, ch = lab.real_trial(lab.send_stream(fn), name + '.jpg')
        lab.log('V', f'{name}.jpg ({fn}): {"shown" if ok else "DROPPED"} ({ch} px changed vs the PREVIOUS FRAME screen)')

def g_I(lab):
    for fn in ('single-A1.bin', 'single-A0.bin', 'single-B1.bin'):
        ok, ch = lab.real_trial(lab.send_stream(fn), fn[:-4] + '.jpg'); lab.log('I', f'{fn}: {"shown" if ok else "dropped"} ({ch} px)')
    fr = lab.frames('single-B1.bin')
    clr = [f for f in fr if f[4] == 182 and f[5] == 2][0]; drw = [f for f in fr if f[4] == 182 and f[5] == 4][0]
    wr = [f for f in fr if f[4] == 182 and f[5] == 3]
    for K in (118, 119):
        def go(K=K):
            lab.fc.write(clr)
            for f in wr[:K]: lab.fc.write(f); time.sleep(0.002)
            lab.fc.write(drw)
        ok, ch = lab.real_trial(go, f'b1-prefix-{K}.jpg'); lab.log('I', f'single-B1 prefix K={K}: {"shown" if ok else "dropped"} ({ch} px)')

def g_attr(lab):
    for name, attrs in (('attr-pages', (0, 1, 2, 3)), ('attr-expected', (0, 0, 0, 0))):
        lab.prev(); lab.fc.clear(); lab.fc.text(1, 2, f'GOGGLES MENU: {lab.a.mode.upper()}')
        for k, at in enumerate(attrs): lab.fc.text(4 + 2 * k, 2, f'ATTR {at}:'); lab.fc.text(4 + 2 * k, 12, 'LOW BATTERY 3.3V', at)
        lab.fc.draw(); time.sleep(1.2); lab.save(name + '.jpg'); lab.log('attr', f'{name}.jpg: "LOW BATTERY 3.3V" attr {attrs} on rows 4,6,8,10')


GROUPS = dict(inavmodel=g_inavmodel, erase=g_erase, align=g_align, cheap=g_cheap, mix=g_mix, checker=g_checker, release=g_release, nul=g_nul, core=g_core, E1=g_E1, G=g_G, C1=g_C1, C2=g_C2, C3=g_C3, C4=g_C4, D1=g_D1, D2=g_D2, D3=g_D3, G1=g_G1, R=g_R, V=g_V, I=g_I, attr=g_attr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('groups', nargs='+', choices=sorted(GROUPS))
    ap.add_argument('--port', default='/dev/ttyUSB0', help='serial port wired to the air unit UART')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--snapshot', default='http://metastreamer/snapshot')
    ap.add_argument('--out', default='fastlab-out')
    ap.add_argument('--streams', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'streams'))
    ap.add_argument('--mode', choices=('bf', 'inav'), default='bf', help='what the goggles menu is set to (names the results file, labels attr pages)')
    ap.add_argument('--prefix', default='', help='prefix for saved images, e.g. inav-')
    ap.add_argument('--n', type=int, default=630, help='G1: WRITEs per glyph code, just under the X limit (INAV mode: 520)')
    ap.add_argument('--limit', type=int, default=637, help='G1: measured X limit for solid rows of 53 (core group; INAV mode: 537)')
    ap.add_argument('--first', type=int, default=0); ap.add_argument('--last', type=int, default=255)
    ap.add_argument('--codes', help='G1: only these glyph codes, e.g. 0x00,0x01,0x7F,0x9C (instead of --first/--last)')
    a = ap.parse_args()
    lab = Lab(a)
    for g in a.groups: GROUPS[g](lab)
    print('done', flush=True)


if __name__ == '__main__':
    main()
