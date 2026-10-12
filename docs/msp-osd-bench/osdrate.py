#!/usr/bin/env python3
"""osdrate: how fast the goggles really redraw the MSP OSD, and whether redraws tear.

The fake FC draws a global draw index as a row of blocks (10 bits) and a moving marker, with CLEAR + WRITE + DRAW at a chosen rate, one rate per segment. Meanwhile the video
from the goggles is captured through metastreamer (HLS, 50 fps). `analyze` reads the blocks back from every
captured frame: distinct counter values per second = real redraw rate; a marker that disagrees with the
counter in the same frame = a torn (partially updated) frame.

  osdrate.py drive   [--rates 5,10,20,30,50,100] [--secs 5]     drive the OSD, write drive.json
  osdrate.py analyze FRAMES_DIR drive.json                        read the captured frames
"""
import argparse, glob, json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from msplab import FC, DP_CLEAR, DP_DRAW   # noqa: E402

BLOCK = 0x8B                     # PB_FULL: a solid cell
BIT_ROW, BIT_COL0, BIT_STEP = 9, 4, 2
MARK_ROW, MARK_COL0, MARK_N = 11, 2, 48
COLS, ROWS = 53, 20


FULL = False   # also rewrite every other row with 53 characters: a full-page update (~1.5 KB per draw)

def draw(fc, seg, n, idx):
    v = idx & 1023
    fc.dp(DP_CLEAR)
    if FULL:
        for r in range(ROWS):
            if r in (7, BIT_ROW, MARK_ROW): continue
            fc.text(r, 0, '#' * 27); fc.text(r, 27, '#' * 26)
    fc.text(7, 2, f'OSDRATE SEG {seg} N {n:3d}')
    for b in range(10):
        if v >> b & 1: fc.text(BIT_ROW, BIT_COL0 + BIT_STEP * b, bytes([BLOCK]))
    fc.text(MARK_ROW, MARK_COL0 + (idx % MARK_N), bytes([BLOCK]))
    fc.dp(DP_DRAW)


def drive(a):
    import serial, threading
    ser = serial.Serial(a.port, a.baud, timeout=0.05)
    fc = FC(ser)
    threading.Thread(target=fc.rx_loop, daemon=True).start()
    threading.Thread(target=fc.heartbeat_loop, daemon=True).start()
    time.sleep(1)
    for _ in range(12):   # warm-up: the goggles take a few seconds to accept a new session, and the capture starts late
        fc.dp(DP_CLEAR); fc.text(7, 2, 'OSDRATE WARMUP'); fc.dp(DP_DRAW); time.sleep(0.5)
    log = []; idx = 0
    rates = [float(r) for r in a.rates.split(',')]
    for seg, rate in enumerate(rates):
        print(f'[seg {seg}] {rate} Hz for {a.secs} s', flush=True)
        t0 = time.time(); n = 0
        while time.time() - t0 < a.secs:
            draw(fc, seg, n, idx)
            log.append((seg, idx, time.time()))
            n += 1; idx += 1
            nxt = t0 + (n / rate)
            time.sleep(max(0, nxt - time.time()))
        print(f'[seg {seg}] sent {n} draws in {time.time() - t0:.1f} s = {n / (time.time() - t0):.1f}/s', flush=True)
    fc.dp(DP_CLEAR); fc.text(7, 2, 'OSDRATE DONE'); fc.dp(DP_DRAW)
    json.dump({'rates': rates, 'secs': a.secs, 'log': log}, open(a.out, 'w'))
    print('wrote', a.out)


def analyze(a):
    from PIL import Image
    d = json.load(open(a.drive))
    seg_of = {idx: seg for seg, idx, _ in d['log']}
    t_of = {idx: t for _, idx, t in d['log']}
    frames = sorted(glob.glob(os.path.join(a.frames, '*.jpg')))
    if not frames: sys.exit('no frames')
    im0 = Image.open(frames[0]); W, H = im0.size
    cw, ch = W / COLS, H / ROWS
    def lit(px, row, col):
        x, y = int((col + 0.5) * cw), int((row + 0.5) * ch)
        return px[x, y] > 128
    def decode(path):
        px = Image.open(path).convert('L').load()
        v = sum(1 << b for b in range(10) if lit(px, BIT_ROW, BIT_COL0 + BIT_STEP * b))
        marks = [i for i in range(MARK_N) if lit(px, MARK_ROW, MARK_COL0 + i)]
        return v, marks
    fps = a.fps
    seen = []   # (frame, idx, torn)
    for k, f in enumerate(frames):
        v, marks = decode(f)
        if v == 0 and not marks: continue
        if v not in seg_of: continue
        seen.append((k, v, marks != [v % MARK_N]))
    if not seen: sys.exit('pattern not found in frames')
    # frame clock -> wall clock: the earliest appearance of each index in segment 0 is the baseline latency
    first = {}
    for k, v, _ in seen: first.setdefault(v, k)
    base_seg = min(seg_of[v] for v in first)
    base = [first[v] / fps - t_of[v] for v in first if seg_of[v] == base_seg]
    off = min(base)   # frame time minus draw time at the lowest captured rate = capture offset + minimal latency
    print(f'{len(frames)} frames at {fps} fps; cell {cw:.1f}x{ch:.1f} px; {len(seen)} frames with the pattern')
    print(f'{"seg":>3} {"sent Hz":>8} {"frames":>6} {"shown":>6} {"shown/s":>8} {"torn":>5} {"extra lag ms":>13} {"tail ms":>8}')
    for seg in range(len(d['rates'])):
        idxs = [v for v in first if seg_of[v] == seg]
        if not idxs: continue
        ks = [k for k, v, _ in seen if seg_of[v] == seg]
        span = (max(ks) - min(ks) + 1) / fps
        torn = sum(1 for k, v, t in seen if seg_of[v] == seg and t)
        lags = sorted(first[v] / fps - t_of[v] - off for v in idxs)
        extra = lags[len(lags) // 2] * 1000
        last_idx = max(v for v in seg_of if seg_of[v] == seg)
        tail = (first[last_idx] / fps - t_of[last_idx] - off) * 1000 if last_idx in first else float('nan')
        print(f'{seg:>3} {d["rates"][seg]:>8.1f} {len(ks):>6} {len(idxs):>6} {len(idxs) / span:>8.1f} {torn:>5} {extra:>13.0f} {tail:>8.0f}')
    print('extra lag: median delay of a draw vs the 5 Hz baseline; tail: delay of the last draw of the segment')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('drive'); p.add_argument('--port', default='/dev/ttyUSB0'); p.add_argument('--baud', type=int, default=115200)
    p.add_argument('--rates', default='5,10,20,30,50,100'); p.add_argument('--secs', type=float, default=5); p.add_argument('--out', default='drive.json')
    p.add_argument('--full', action='store_true', help='full-page updates (every row rewritten)')
    p = sub.add_parser('analyze'); p.add_argument('frames'); p.add_argument('drive'); p.add_argument('--fps', type=float, default=50)
    a = ap.parse_args()
    global FULL
    FULL = getattr(a, 'full', False)
    (drive if a.cmd == 'drive' else analyze)(a)


if __name__ == '__main__':
    main()
