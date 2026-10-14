"""cms_usb.py OUT steps...: drive CMS with MSP_SET_RAW_RC over USB (RX_MSP), snapshot after every step. No sniffer."""
import sys, time, serial, urllib.request, threading, os, struct, pickle
FC, SNAP = '/dev/cu.usbmodem205C327F34561', 'http://192.168.11.164/snapshot'
OUT = sys.argv[1]; os.makedirs(OUT, exist_ok=True)
MID = [1500, 1500, 1500, 1500, 1000, 1000, 1000, 1000]   # AETR: roll, pitch, throttle, yaw, aux
sticks = list(MID); run = True
u = serial.Serial(FC, 115200, timeout=0.05)


def frame(cmd, payload):
    c = len(payload) ^ cmd
    for b in payload: c ^= b
    return b'$M<' + bytes([len(payload), cmd]) + payload + bytes([c])


def rc():
    while run:
        u.write(frame(200, struct.pack('<8H', *sticks))); u.read(64); time.sleep(0.02)


def snap(name):
    try: open(OUT + name + '.jpg', 'wb').write(urllib.request.urlopen(SNAP, timeout=3).read())
    except Exception as e: print('snapshot failed', name, e)


def hold(name, **kw):
    global sticks
    sticks = list(MID)
    for k, v in kw.items(): sticks[{'roll': 0, 'pitch': 1, 'thr': 2, 'yaw': 3}[k]] = v
    time.sleep(0.4 if kw else 0.6)
    sticks = list(MID); time.sleep(1.0)
    log.append((time.time(), name)); snap(name); print(name, flush=True)


data = []; log = []; t0 = time.time()
threading.Thread(target=rc, daemon=True).start()
time.sleep(2); snap('00-start')
# steps from argv: name=stick:value[,stick:value][@hold_s]  e.g. enter=yaw:1000,pitch:2000@1.5  down=pitch:1000
for k, st in enumerate(sys.argv[2:]):
    name, _, spec = st.partition('=')
    spec, _, h = spec.partition('@')
    sticks = list(MID)
    for kv in filter(None, spec.split(',')):
        a, v = kv.split(':'); sticks[{'roll': 0, 'pitch': 1, 'thr': 2, 'yaw': 3}[a]] = int(v)
    time.sleep(float(h or 0.4)); sticks = list(MID); time.sleep(1.2)
    log.append((time.time(), name)); snap(f'{k+1:02d}-{name}'); print(k + 1, name, flush=True)
run = False; time.sleep(0.3)
pickle.dump({'t0': t0, 'log': log}, open(OUT + 'cms.pkl', 'wb'))
