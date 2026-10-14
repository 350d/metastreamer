#!/usr/bin/env python3
"""budget_model: the DisplayPort frame budget of O3 + Goggles 2 (Betaflight mode), fitted 2026-10-14.

Model: the 53x20 char buffer (row-major, 1 byte per cell, 0 = empty) is read as 265 little-endian u32 words,
each word LEB128-encoded; a frame is dropped when the encoded size exceeds 902 bytes.
Run it to print the encoded size at Nmax (shown) and Nmax+1 (dropped) for every bench measurement.
"""
import sys
COLS, ROWS = 53, 20
def leb(v):
    n = 1
    while v >= 128: v >>= 7; n += 1
    return n
def size_bf(cells):                       # cells: list of (r, c, code)
    buf = bytearray(COLS * ROWS)
    for r, c, ch in cells: buf[r * COLS + c] = ch
    buf += bytes(-len(buf) % 4)
    return sum(leb(int.from_bytes(buf[i:i + 4], 'little')) for i in range(0, len(buf), 4))
def size_inav(cells):                     # INAV mode guess: u16 per cell
    buf = bytearray(COLS * ROWS * 2)
    for r, c, ch in cells: buf[2 * (r * COLS + c)] = ch
    return sum(leb(int.from_bytes(buf[i:i + 4], 'little')) for i in range(0, len(buf), 4))
X = 0x58
solid = lambda w, c0, n, code=X: [(k // w, c0 + k % w, code) for k in range(n)]
iso = lambda step, n: [(r, c, X) for r in range(19) for c in range(0, 53, step)][:n]
iso26 = lambda n: [(r, 2 * c, X) for r in range(19) for c in range(26)][:n]
checker = lambda n: [(r, 2 * c + r % 2, X) for r in range(19) for c in range(26)][:n]
def runs(L, gap, n):
    out = []
    for r in range(19):
        c = 0
        while c + L <= 53:
            out += [(r, c + i, X) for i in range(L)]; c += L + gap
    return out[:n]
pairs17 = lambda n: [(r, 3 * k + i, X) for r in range(19) for k in range(17) for i in (0, 1)][:n]
def mix(f, n): return [(k // 53, k % 53, f(k, k // 53, k % 53)) for k in range(n)]
def aligned(off, n): return [(L // 53, L % 53, X) for m in range(19 * 53 // 4) for L in (4 * m + off, 4 * m + off + 1) if L < 19 * 53][:n]
def main():
    cases = [  # name, builder(n), Nmax shown (Nmax+1 dropped), source
     ('solid53 X', lambda n: solid(53, 0, n), 637), ('solid50@2 X', lambda n: solid(50, 2, n), 620),
     ('solid40@0', lambda n: solid(40, 0, n), 618), ('solid40@13', lambda n: solid(40, 13, n), 617),
     ('solid50@0', lambda n: solid(50, 0, n), 622), ('solid50@3', lambda n: solid(50, 3, n), 619),
     ('solid51@1', lambda n: solid(51, 1, n), 621), ('solid52@0', lambda n: solid(52, 0, n), 629),
     ('solid53 0x01', lambda n: solid(53, 0, n, 1), 850), ('solid50@2 0x01', lambda n: solid(50, 2, n, 1), 832),
     ('solid53 0x07', lambda n: solid(53, 0, n, 7), 851),
     ('solid50@2 0x30', lambda n: solid(50, 2, n, 0x30), 624), ('solid50@2 0x90', lambda n: solid(50, 2, n, 0x90), 617),
     ('iso step2 27/row', lambda n: iso(2, n), 371), ('iso26 stripes', iso26, 361), ('checker', checker, 425),
     ('iso step3', lambda n: iso(3, n), 287),
     ('runs1', lambda n: runs(1, 1, n), 371), ('runs2', lambda n: runs(2, 1, n), 471), ('runs3', lambda n: runs(3, 1, n), 512),
     ('runs5', lambda n: runs(5, 1, n), 563), ('runs10', lambda n: runs(10, 1, n), 586), ('runs25', lambda n: runs(25, 1, n), 613),
     ('pairs gap1 17/row', pairs17, 459), ('pairs gap3', lambda n: runs(2, 3, n), 396),
     ('mix k%2 0x01 odd', lambda n: mix(lambda k, r, c: 1 if k % 2 else X, n), 850),
     ('mix k%2 X odd', lambda n: mix(lambda k, r, c: X if k % 2 else 1, n), 637),
     ('mix k%4==0 0x01', lambda n: mix(lambda k, r, c: 1 if k % 4 == 0 else X, n), 637),
     ('mix k%4!=0 0x01', lambda n: mix(lambda k, r, c: 1 if k % 4 else X, n), 850),
     ('mix even cols 0x01', lambda n: mix(lambda k, r, c: 1 if c % 2 == 0 else X, n), 729),
     ('mix even rows 0x01', lambda n: mix(lambda k, r, c: 1 if r % 2 == 0 else X, n), 729),
     ('mix cols<27 0x01', lambda n: mix(lambda k, r, c: 1 if c < 27 else X, n), 731),
     ('mix rows<9 0x01', lambda n: mix(lambda k, r, c: 1 if r < 9 else X, n), 757),
     ('mix 0x10/X', lambda n: mix(lambda k, r, c: 0x10 if k % 2 else X, n), 638),
     ('mix 0x0F/0x10', lambda n: mix(lambda k, r, c: 0x0F if k % 2 else 0x10, n), 850),
     ('align off1', lambda n: aligned(1, n), 424), ('align off2', lambda n: aligned(2, n), 318), ('align off3', lambda n: aligned(3, n), 318),
    ]
    for name, b, nmax in cases:
        print(f'{name:22s} Nmax {nmax:4d}: size(shown) {size_bf(b(nmax)):4d}  size(dropped) {size_bf(b(nmax + 1)):4d}')
    print('align off0 all 502:', size_bf(aligned(0, 502)))
    print('INAV (u16/cell guess):')
    for name, b, nmax in [('solid53', lambda n: solid(53, 0, n), 537), ('solid50@2', lambda n: solid(50, 2, n), 526),
                          ('iso26', iso26, 326), ('pairs17', pairs17, 407)]:
        print(f'  {name:12s} Nmax {nmax}: bf-model {size_bf(b(nmax))}/{size_bf(b(nmax+1))}  u16-model {size_inav(b(nmax))}/{size_inav(b(nmax+1))}')


if __name__ == '__main__':
    main()
