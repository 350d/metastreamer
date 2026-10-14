"""motion_usb.py OUT SECS [DELAY]: no sniffer. While the FC is moved by hand: goggles snapshots 2/s, and over USB MSP
the attitude and the MSP_DISPLAYPORT debug values (bytes and cells of the last frame, DJI screen load, writes held)."""
import sys, time, serial, urllib.request, io, threading, os, pickle, struct
from PIL import Image
FC, SNAP = '/dev/cu.usbmodem205C327F34561', 'http://192.168.11.164/snapshot'
OUT = sys.argv[1]; SECS = float(sys.argv[2]); DELAY = float(sys.argv[3]) if len(sys.argv) > 3 else 0
os.makedirs(OUT, exist_ok=True)


def lit_cells(im):
    W, H = im.size; cw, ch = W / 53, H / 20; out = set()
    for r in range(20):
        for c in range(53):
            if (r >= 15 and c >= 35) or (5 <= r <= 7 and c >= 44): continue
            x0, y0 = int(c * cw) + 3, int(r * ch) + 5
            if max(im.getpixel((x, y)) for x in range(x0, int((c + 1) * cw) - 3, 3) for y in range(y0, int((r + 1) * ch) - 5, 3)) > 110: out.add((r, c))
    return out



def msp(u, cmd):
    u.reset_input_buffer(); u.write(b'$M<' + bytes([0, cmd, cmd])); end = time.time() + 0.3; r = b''
    while time.time() < end:
        r += u.read(128); i = r.find(b'$M>')
        if i >= 0 and len(r) >= i + 6 + r[i + 3]: return r[i + 5:i + 5 + r[i + 3]]
    return None


time.sleep(DELAY)
print('GO', flush=True)
stop = time.time() + SECS; snaps = []; att = []; dbg = []
def shots():
    while time.time() < stop:
        t = time.time()
        try: snaps.append((t, urllib.request.urlopen(SNAP, timeout=3).read()))
        except Exception: pass
        time.sleep(max(0, 0.5 - (time.time() - t)))
th = threading.Thread(target=shots, daemon=True); th.start()
u = serial.Serial(FC, 115200, timeout=0.05)
while time.time() < stop:
    a = msp(u, 108)
    if a and len(a) >= 6: att.append((time.time(),) + struct.unpack('<hhh', a[:6]))
    d = msp(u, 254)
    if d and len(d) >= 8: dbg.append((time.time(),) + struct.unpack('<4h', d[:8]))
    time.sleep(0.05)
th.join(5)
pickle.dump({'snaps': snaps, 'att': att, 'dbg': dbg}, open(OUT + 'motion.pkl', 'wb'))
cells = [len(lit_cells(Image.open(io.BytesIO(b)).convert('L'))) for _, b in snaps]
if snaps: open(OUT + 'mid.jpg', 'wb').write(snaps[len(snaps) // 2][1])
full = max(cells, default=0); roll = [x[1] / 10 for x in att]
load = sorted(x[3] for x in dbg) or [0]; byt = sorted(x[1] for x in dbg) or [0]
lines = [f'snapshots {len(cells)}: lit cells min {min(cells, default=0)} max {full}; complete (>= max-25) {sum(c >= full - 25 for c in cells)}',
         f'roll {min(roll, default=0):.0f}..{max(roll, default=0):.0f} deg over {len(att)} samples',
         f'debug {len(dbg)} samples: DJI load median {load[len(load)//2]} max {load[-1]} B (budget 902); bytes of last frame median {byt[len(byt)//2]} max {byt[-1]}; writes held max {max((x[4] for x in dbg), default=0)}']
print('\n'.join(lines)); open(OUT + 'results.txt', 'w').write('\n'.join(lines) + '\n')
