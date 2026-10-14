#!/usr/bin/env python3
"""msplab: a fake flight controller on the air unit's UART, plus MSP DisplayPort experiments checked by snapshots.

The air unit (DJI O3/O4) polls the "flight controller" over MSP. msplab answers like Betaflight 4.5, disarmed,
keeps a DisplayPort session alive, and runs experiments: every step draws something, waits, grabs the frame
from metastreamer's /snapshot (the goggles' picture as the Pi receives it) and saves it, then a contact sheet
per experiment shows all steps at once.

  msplab.py serve                         answer polls, show a marker in the OSD
  msplab.py run voltage [--out DIR]       which reply the OSD voltage comes from
  msplab.py run subcmd|attr|sys|grid|font

Needs pyserial and Pillow. STATUS reports disarmed unless an experiment sets FC.armed.
"""
import argparse, os, struct, sys, threading, time, urllib.request

MSP_REQ, MSP_REP = ord('<'), ord('>')
# MSP commands
API_VERSION, FC_VARIANT, FC_VERSION, BOARD_INFO, BUILD_INFO, NAME = 1, 2, 3, 4, 5, 10
OSD_CONFIG, FILTER_CONFIG, PID_ADVANCED, STATUS, RC, RAW_GPS, COMP_GPS = 84, 92, 94, 101, 105, 106, 107
ATTITUDE, ALTITUDE, ANALOG, RC_TUNING, PID, BATTERY_STATE, ESC_SENSOR = 108, 109, 110, 111, 112, 130, 134
STATUS_EX, DISPLAYPORT, SET_OSD_CANVAS, OSD_CANVAS, RTC = 150, 182, 188, 189, 247
# DisplayPort sub-commands
DP_HEARTBEAT, DP_RELEASE, DP_CLEAR, DP_WRITE, DP_DRAW, DP_OPTIONS, DP_SYS = range(7)

NAMES = {v: k for k, v in globals().items() if k.isupper() and isinstance(v, int) and not k.startswith(('DP_', 'MSP_'))}


def crc(b):
    c = 0
    for x in b: c ^= x
    return c


def frame(direction, cmd, data=b''):
    body = bytes([len(data), cmd]) + bytes(data)
    return b'$M' + bytes([direction]) + body + bytes([crc(body)])


class FC:
    """Answers the air unit's polls; experiments change self.tele or self.override at run time."""

    def __init__(self, ser, hd=True):
        self.ser, self.hd = ser, hd
        self.wlock = threading.Lock()
        self.seen, self.slock = {}, threading.Lock()
        # telemetry the replies are built from
        self.tele = dict(cells=4, capacity=1500, vbat_cv=1680, mah=0, amps_ca=0, rssi=0)
        self.override = {}   # cmd -> raw reply bytes, for format experiments
        self.variant = b'BTFL'
        self.mute = False    # stop answering: the air unit then treats the FC as gone
        self.armed = False   # STATUS flightModeFlags bit 0: the O3 leaves low-power only on a disarmed -> armed edge

    def write(self, b):
        with self.wlock:
            self.ser.write(b)

    def reply_data(self, cmd):
        if cmd in self.override: return self.override[cmd]
        t = self.tele
        if cmd == API_VERSION: return bytes([0, 1, 46])                    # MSP protocol 0, API 1.46
        if cmd == FC_VARIANT: return self.variant
        if cmd == FC_VERSION: return bytes([4, 5, 0])
        if cmd == NAME: return b'MSPLAB'
        if cmd in (STATUS, STATUS_EX):
            # Betaflight layout, as ExpressLRS SerialDisplayport sends it (22 bytes): cycleTime u16, i2cErrors u16,
            # sensors u16, flightModeFlags u32 (bit 0 = armed), pidProfile u8, load u16, gyroCycle u16,
            # extra mode-flag bytes u8, arming-disable count u8, arming-disable flags u32, config state u8
            return struct.pack('<HHHIBHHBBIB', 125, 0, 0x23, 1 if self.armed else 0, 0, 0, 0, 0, 1, 0 if self.armed else 1, 0)
        if cmd == ANALOG:
            # Betaflight: vbat u8 (0.1 V), mAh u16, rssi u16, amperage i16 (0.01 A), vbat u16 (0.01 V)
            return struct.pack('<BHHhH', min(255, t['vbat_cv'] // 10), t['mah'], t['rssi'], t['amps_ca'], t['vbat_cv'])
        if cmd == BATTERY_STATE:
            # cells u8, capacity u16, vbat u8 (0.1 V), mAh drawn u16, amperage u16 (0.01 A), state u8, vbat u16 (0.01 V)
            return struct.pack('<BHBHHBH', t['cells'], t['capacity'], min(255, t['vbat_cv'] // 10), t['mah'],
                               t['amps_ca'] & 0xffff, 0, t['vbat_cv'])
        if cmd == OSD_CONFIG: return bytes([0x40, 3 if self.hd else 1, 0, 0, 0, 0, 0])
        if cmd == OSD_CANVAS: return bytes([53, 20]) if self.hd else bytes([30, 16])
        return b''   # empty but valid: enough for the rest so far

    def on_request(self, cmd, payload):
        with self.slock:
            self.seen[cmd] = self.seen.get(cmd, 0) + 1
            first = self.seen[cmd] == 1
        if first:
            print(f'[poll] new: {cmd} {NAMES.get(cmd, "?")} payload={payload.hex()}', flush=True)
        if not self.mute:
            self.write(frame(MSP_REP, cmd, self.reply_data(cmd)))

    def rx_loop(self):
        st, ln, cmd, data, ck = 0, 0, 0, bytearray(), 0
        while True:
            for b in self.ser.read(256):
                if st == 0: st = 1 if b == ord('$') else 0
                elif st == 1: st = 2 if b == ord('M') else 0
                elif st == 2: st = 3 if b == MSP_REQ else 0
                elif st == 3: ln, data, ck, st = b, bytearray(), b, 4
                elif st == 4: cmd, ck, st = b, ck ^ b, (5 if ln else 6)
                elif st == 5:
                    data.append(b); ck ^= b
                    if len(data) >= ln: st = 6
                elif st == 6:
                    st = 0
                    if b == ck: self.on_request(cmd, bytes(data))

    # DisplayPort
    def dp(self, sub, data=b''): self.write(frame(MSP_REP, DISPLAYPORT, bytes([sub]) + bytes(data)))
    def clear(self): self.dp(DP_CLEAR)
    def draw(self): self.dp(DP_DRAW)
    def text(self, row, col, s, attr=0, limit=30):
        b = s.encode('latin-1') if isinstance(s, str) else bytes(s)
        self.dp(DP_WRITE, bytes([row, col, attr]) + b[:limit])

    hb = True
    def heartbeat_loop(self):
        while True:
            if self.hb: self.dp(DP_HEARTBEAT)
            time.sleep(0.5)


def snapshot(url, path, tries=3):
    for _ in range(tries):
        try:
            data = urllib.request.urlopen(url, timeout=4).read()
            open(path, 'wb').write(data)
            return True
        except Exception as e:
            err = e; time.sleep(0.5)
    print(f'[snap] failed: {err}', flush=True)
    return False


def contact_sheet(items, out, cols=4, crop=None):
    """items: [(path, label)]; crop: (x0, y0, x1, y1) in frame pixels, to look closer at a region"""
    from PIL import Image, ImageDraw
    tw = 480
    thumbs = []
    for path, label in items:
        try: im = Image.open(path).convert('RGB')
        except Exception: im = Image.new('RGB', (1920, 1080), (60, 0, 0))
        if crop: im = im.crop(crop)
        th = int(im.height * tw / im.width)
        thumbs.append((im.resize((tw, th)), label))
    th = thumbs[0][0].height
    rows = (len(thumbs) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * tw, rows * (th + 22)), (30, 30, 30))
    d = ImageDraw.Draw(sheet)
    for i, (im, label) in enumerate(thumbs):
        x, y = (i % cols) * tw, (i // cols) * (th + 22)
        sheet.paste(im, (x, y + 22))
        d.text((x + 4, y + 4), label[:70], fill=(255, 220, 120))
    sheet.save(out)
    print(f'[sheet] {out}', flush=True)


# ---------------- experiments: each is a list of (label, action) ----------------

def exp_voltage(fc):
    def old():
        fc.override = {ANALOG: bytes([0x58, 0, 0, 0, 0, 0, 0]), BATTERY_STATE: bytes([6, 0, 0, 0, 0, 0x58, 0, 0, 0, 0])}
    def analog_only():   # Betaflight ANALOG, BATTERY_STATE empty
        fc.override = {BATTERY_STATE: b''}; fc.tele['vbat_cv'] = 1234
    def battery_only():  # Betaflight BATTERY_STATE, ANALOG empty
        fc.override = {ANALOG: b''}; fc.tele['vbat_cv'] = 2345
    def both_differ():   # both, with different values: which one does the OSD show?
        fc.override = {ANALOG: struct.pack('<BHHhH', 123, 0, 0, 0, 1234)}
        fc.tele['vbat_cv'] = 2345
    def legacy_only():   # only the 0.1 V byte filled, 0.01 V fields zero
        fc.override = {ANALOG: struct.pack('<BHHhH', 111, 0, 0, 0, 0), BATTERY_STATE: struct.pack('<BHBHHBH', 4, 1500, 111, 0, 0, 0, 0)}
    def cells3():
        fc.override = {}; fc.tele.update(vbat_cv=1110, cells=3)
    return [('old 7/10-byte replies (8.8 V?)', old), ('ANALOG 12.34 V, BATTERY empty', analog_only),
            ('BATTERY 23.45 V, ANALOG empty', battery_only), ('ANALOG 12.34 vs BATTERY 23.45', both_differ),
            ('only the 0.1 V bytes: 11.1 V', legacy_only), ('both 11.10 V, 3 cells', cells3)]


def exp_subcmd(fc):
    steps = []
    for sub in range(7, 32):
        def act(sub=sub):
            fc.clear(); fc.text(2, 2, f'SUB {sub}'); fc.dp(sub, bytes([6, 6, 0]) + b'XYZ'); fc.draw()
        steps.append((f'subcmd {sub} + [6,6,0,"XYZ"]', act))
    return steps


def exp_attr(fc):
    steps = []
    for a in (0, 1, 2, 3, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x81, 0xC0, 0xFF):
        def act(a=a):
            fc.clear(); fc.text(2, 2, f'ATTR 0x{a:02X}'); fc.text(6, 2, 'ABCabc 0123 #%&*', a); fc.draw()
        steps.append((f'attr 0x{a:02X}', act, 4 if a & 0x40 else 1))
    return steps


def exp_sys(fc):
    steps = []
    for el in range(0, 24):
        def act(el=el):
            fc.clear(); fc.text(2, 2, f'SYS {el}'); fc.dp(DP_SYS, bytes([6, 6, el])); fc.draw()
        steps.append((f'sys element {el} at 6,6', act))
    return steps


def exp_grid(fc):
    """how far the canvas reaches: markers on rows and columns beyond 53x20"""
    def rows():
        fc.clear()
        for r in range(0, 26): fc.text(r, 0, f'R{r}')
        fc.draw()
    def cols():
        fc.clear()
        for c in range(0, 64, 4): fc.text(10, c, f'{c}')
        fc.draw()
    def corner():
        fc.clear(); fc.text(0, 0, 'TL'); fc.text(0, 51, 'TR'); fc.text(19, 0, 'BL'); fc.text(19, 51, 'BR')
        fc.text(20, 0, 'R20'); fc.text(21, 0, 'R21'); fc.text(5, 53, '<53'); fc.text(6, 55, '<55'); fc.draw()
    return [('rows 0..25 at col 0', rows), ('columns 0..60 on row 10', cols), ('corners of 53x20 and beyond', corner)]


def exp_font(fc):
    """the goggles' font: every code 0..255 on font pages 0..3"""
    steps = []
    for page in range(4):
        def act(page=page):
            fc.clear(); fc.text(0, 0, f'PAGE {page}')
            for r in range(16):   # 16 codes per row, rows 2..17
                fc.text(2 + r, 2, bytes(range(r * 16, r * 16 + 16)), page)
            fc.draw()
        steps.append((f'font page {page}', act))
    return steps


# region of the frame to show in the contact sheet (None = whole frame)
CROPS = dict(v2=(0, 0, 1280, 540), hbtimeout=(0, 0, 1280, 540), strlen=(0, 0, 1920, 540), persist=(0, 0, 1280, 540), attr=(0, 0, 1280, 720), subcmd=(0, 0, 1280, 720), font=(0, 0, 1280, 1080), fontchar=(0, 0, 1280, 720),
             variant=(0, 0, 1280, 720))

def glyph_bytes(pattern):
    """MAX7456 character, 12x18 pixels, 2 bits each (10 white, 00 black, 01 transparent), 54 bytes + 10 padding"""
    px = []
    for y in range(18):
        for x in range(12):
            px.append(0b10 if pattern(x, y) else 0b00)
    out = bytearray()
    for i in range(0, len(px), 4):
        out.append(px[i] << 6 | px[i + 1] << 4 | px[i + 2] << 2 | px[i + 3])
    return bytes(out) + bytes(10)


def exp_fontchar(fc):
    def sample():
        fc.clear(); fc.text(2, 2, 'FONTCHAR WRITE')
        fc.text(5, 2, 'AB AB @ 0123'); fc.text(7, 2, bytes([0x41, 0x42, 0x01, 0x02, 0x03])); fc.draw()
    def write(code, pattern, hi=0):
        fc.dp(7, bytes([code, hi, 0]) + glyph_bytes(pattern)); time.sleep(0.1)
    def base(): sample()
    def a_white(): write(0x41, lambda x, y: True); sample()
    def b_checker(): write(0x42, lambda x, y: (x // 2 + y // 2) % 2 == 0); sample()
    def page1(): write(0x41, lambda x, y: x == y // 1.5, hi=1); sample()
    def many(): [write(c, lambda x, y: x < 6) for c in range(0x30, 0x34)]; sample()
    return [('baseline: AB AB @ 0123', base), ('write A (0x41) = white block', a_white),
            ('write B (0x42) = checker', b_checker), ('write A, addr high byte 1', page1),
            ('write 0..3 = left half', many)]


def exp_options(fc):
    steps = []
    def markers(tag):
        fc.clear(); fc.text(0, 0, tag)
        fc.text(19, 0, 'R19'); fc.text(20, 0, 'R20'); fc.text(21, 0, 'R21')
        fc.text(5, 49, 'C49'); fc.text(6, 53, 'C53'); fc.text(7, 57, 'C57'); fc.text(8, 50, '50-59 0123456789')
        fc.draw()
    for res in range(0, 6):
        def act(res=res):
            fc.dp(DP_OPTIONS, bytes([0, res])); time.sleep(0.2); markers(f'OPTIONS font 0 res {res}')
        steps.append((f'OPTIONS font=0 resolution={res}', act))
    for font in range(1, 4):
        def act(font=font):
            fc.dp(DP_OPTIONS, bytes([font, 4])); time.sleep(0.2)
            fc.clear(); fc.text(0, 0, f'OPTIONS font {font} res 4'); fc.text(3, 2, bytes(range(0x01, 0x1f))); fc.text(5, 2, 'abcdefghijklmnop'); fc.draw()
        steps.append((f'OPTIONS font={font} resolution=4', act))
    return steps


def exp_variant(fc):
    """answer a different FC_VARIANT; go silent first so the air unit asks again"""
    steps = []
    def sample(tag):
        fc.clear(); fc.text(0, 0, tag); fc.text(2, 2, bytes(range(0x01, 0x1f))); fc.text(4, 2, 'abcdefghijklmnopqrstuvwxyz')
        fc.text(6, 2, bytes(range(0x80, 0x9e))); fc.text(8, 2, bytes(range(0xa0, 0xbe))); fc.draw()
    for v in (b'INAV', b'ARDU', b'BTFL'):
        def act(v=v):
            with fc.slock: before = dict(fc.seen)
            fc.mute = True; time.sleep(8); fc.variant = v; fc.mute = False; time.sleep(4)
            with fc.slock: asked = sorted(NAMES.get(k, k) for k in fc.seen if fc.seen[k] != before.get(k, 0) and k in (API_VERSION, FC_VARIANT, FC_VERSION, BOARD_INFO, BUILD_INFO))
            print(f'[variant {v.decode()}] asked after silence: {asked}', flush=True)
            sample(f'VARIANT {v.decode()}')
        steps.append((f'FC_VARIANT {v.decode()} after 8 s silence', act))
    return steps


ATLAS_COL0, ATLAS_STEP, ATLAS_ROW0 = 5, 3, 2   # code c at row ATLAS_ROW0 + c//16, col ATLAS_COL0 + 3*(c%16)

def exp_atlas(fc, page=0):
    def act():
        fc.clear()
        for r in range(16):
            fc.text(ATLAS_ROW0 + r, 0, f'{r:X}0')
            for i in range(16):
                c = r * 16 + i
                if c: fc.text(ATLAS_ROW0 + r, ATLAS_COL0 + ATLAS_STEP * i, bytes([c]), page)
        fc.draw()
    return [(f'labelled atlas, codes 1..255, font page {page}', act)]

def exp_atlas1(fc): return exp_atlas(fc, 1)


def exp_strlen(fc):
    """how long a WRITE string may be, and whether a long one fills the following rows (linear buffer)"""
    steps = []
    for n in (30, 31, 40, 53, 54, 60, 106, 159, 250):
        def act(n=n):
            fc.clear(); fc.text(1, 2, f'STRLEN {n}')
            s = ''.join(chr(0x30 + (i % 10)) for i in range(n))          # 0123456789 repeated
            fc.text(3, 0, s, limit=255); fc.draw()
        steps.append((f'write {n} chars at row 3 col 0', act))
    return steps


def exp_persist(fc):
    """does content survive DRAW without CLEAR, and how long after the last heartbeat"""
    def a1(): fc.clear(); fc.text(2, 2, 'PERSIST A'); fc.text(4, 2, 'ROW4 ORIGINAL'); fc.draw()
    def a2(): fc.text(6, 2, 'ROW6 ADDED NO CLEAR'); fc.draw()
    def a3(): fc.text(4, 2, 'ROW4 OVERWRITE'); fc.draw()
    def a4(): fc.text(4, 2, ' '); fc.draw()                           # single space over the first char
    def a5(): fc.draw()
    def a6(): fc.dp(DP_RELEASE)
    return [('clear + rows 2,4', a1), ('+ row 6, no clear', a2), ('overwrite row 4', a3),
            ('space at row4 col2', a4), ('draw only', a5), ('RELEASE', a6)]


def exp_canvas(fc):
    """push MSP_SET_OSD_CANVAS (188) and MSP_OSD_CANVAS (189) with other sizes, then mark the edges"""
    def mark(tag):
        fc.clear(); fc.text(0, 0, tag); fc.text(19, 0, 'R19'); fc.text(20, 0, 'R20'); fc.text(21, 0, 'R21')
        fc.text(5, 50, '50X'); fc.text(6, 53, '53X'); fc.text(7, 57, '57X'); fc.draw()
    steps = [('baseline', lambda: mark('CANVAS BASE'))]
    for cols, rows in ((60, 22), (50, 18), (30, 16), (64, 32)):
        def act(cols=cols, rows=rows):
            fc.write(frame(MSP_REP, SET_OSD_CANVAS, bytes([cols, rows])))
            fc.write(frame(MSP_REP, OSD_CANVAS, bytes([cols, rows])))
            fc.override[OSD_CANVAS] = bytes([cols, rows])
            time.sleep(0.5); mark(f'CANVAS {cols}X{rows}')
        steps.append((f'push canvas {cols}x{rows}', act))
    return steps


def exp_flags(fc):
    """flight mode flags (never bit 0 = ARM) and arming-disable flags in STATUS/STATUS_EX, battery state byte"""
    steps = []
    def status(mode_flags, arm_dis=0):
        # STATUS: cycle u16, i2c u16, sensors u16, modeflags u32, profile u8, load u16, gyrocycle u16, boxcount u8, boxes..., armingDisableCount u8, armingDisableFlags u32
        return struct.pack('<HHHIBHHB', 125, 0, 0x23, mode_flags, 0, 100, 125, 0) + struct.pack('<BI', 22, arm_dis)
    def base(): fc.override = {}; fc.clear(); fc.text(2, 2, 'FLAGS BASE'); fc.draw()
    steps.append(('baseline', base))
    for bit in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17):
        def act(bit=bit):
            fc.override = {STATUS: status(1 << bit), STATUS_EX: status(1 << bit)}
            fc.clear(); fc.text(2, 2, f'MODE BIT {bit}'); fc.draw()
        steps.append((f'flight mode bit {bit}', act))
    for bit in (0, 1, 2, 3, 4, 7, 9, 10, 11, 16, 17, 18, 19, 20, 21):
        def act(bit=bit):
            fc.override = {STATUS: status(0, 1 << bit), STATUS_EX: status(0, 1 << bit)}
            fc.clear(); fc.text(2, 2, f'ARMDIS BIT {bit}'); fc.draw()
        steps.append((f'arming disable bit {bit}', act))
    for st in (1, 2, 3, 4):
        def act(st=st):
            fc.override = {BATTERY_STATE: struct.pack('<BHBHHBH', 4, 1500, 148, 0, 0, st, 1480)}
            fc.clear(); fc.text(2, 2, f'BATT STATE {st}'); fc.draw()
        steps.append((f'battery state {st}', act, 3))
    steps.append(('restore', base))
    return steps


def frame_v2(cmd, data=b''):
    """MSP v2 frame: $X> flag u8, cmd u16, len u16, payload, crc8 (DVB-S2)"""
    body = bytes([0]) + struct.pack('<HH', cmd, len(data)) + data
    c = 0
    for b in body:
        c ^= b
        for _ in range(8): c = ((c << 1) ^ 0xD5) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return b'$X>' + body + bytes([c])


def exp_v2(fc):
    def v1(): fc.clear(); fc.text(2, 2, 'V1 FRAME'); fc.draw()
    def v2():
        fc.write(frame_v2(DISPLAYPORT, bytes([DP_CLEAR])))
        fc.write(frame_v2(DISPLAYPORT, bytes([DP_WRITE, 4, 2, 0]) + b'V2 FRAME OK'))
        fc.write(frame_v2(DISPLAYPORT, bytes([DP_DRAW])))
    def v2long():
        fc.write(frame_v2(DISPLAYPORT, bytes([DP_WRITE, 6, 0, 0]) + ''.join(chr(0x30 + i % 10) for i in range(53)).encode()))
        fc.write(frame_v2(DISPLAYPORT, bytes([DP_DRAW])))
    return [('v1 baseline', v1), ('v2 clear+write+draw', v2), ('v2 write 53 chars row 6', v2long)]


def exp_hbtimeout(fc):
    """stop heartbeats: when do the goggles drop the OSD?"""
    def start(): fc.hb = True; fc.clear(); fc.text(2, 2, 'HB TIMEOUT TEST'); fc.draw(); fc.hb = False
    steps = [('heartbeat stopped, t=0', start)]
    for t in (3, 6, 10, 15, 20, 30):
        steps.append((f'no heartbeat for {t} s', lambda: time.sleep(0)))
    def back(): fc.hb = True; time.sleep(2); fc.text(4, 2, 'HB BACK'); fc.draw()
    steps.append(('heartbeat back + write', back))
    return steps


EXPERIMENTS = dict(voltage=exp_voltage, subcmd=exp_subcmd, attr=exp_attr, sys=exp_sys, grid=exp_grid, font=exp_font,
                   fontchar=exp_fontchar, options=exp_options, variant=exp_variant, atlas=exp_atlas,
                   strlen=exp_strlen, persist=exp_persist, canvas=exp_canvas, flags=exp_flags, v2=exp_v2, hbtimeout=exp_hbtimeout, atlas1=exp_atlas1)


def main():
    import serial
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', choices=['serve', 'run', 'demo'])
    ap.add_argument('experiment', nargs='?', choices=sorted(EXPERIMENTS))
    ap.add_argument('--port', default='/dev/cu.usbserial-0001')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--snap', default='http://192.168.11.164/snapshot')
    ap.add_argument('--out', default='msplab-out')
    ap.add_argument('--dwell', type=float, default=1.5, help='seconds between drawing and the snapshot')
    ap.add_argument('--effect', choices=['scroller', 'matrix'], default='scroller', help='demo: the effect')
    ap.add_argument('--text', help='demo scroller: the message')
    ap.add_argument('--budget', type=int, help='demo matrix: load the rain is kept under (default: displayport_msp_cell_budget - 20)')
    ap.add_argument('--no-safe', action='store_true', help='demo: draw over the goggles\' own UI too (no safe zone)')
    ap.add_argument('--fps', type=float, help='demo: frames a second (default by effect: scroller 12, matrix 6; a frame is 0.5-1 KB, UART-bound)')
    ap.add_argument('--step', type=int, default=1, help='demo: cells the text moves a frame (speed = fps * step cells a second)')
    ap.add_argument('--seconds', type=float, default=0, help='demo: stop after (0 = until Ctrl-C)')
    ap.add_argument('--grab', type=int, default=0, help='demo: snapshots to save into --out, spread over the run')
    ap.add_argument('--sd', action='store_true', help='report SD video instead of HD')
    ap.add_argument('--variant', default='BTFL', help='FC_VARIANT to report (BTFL, INAV, ARDU, ...)')
    a = ap.parse_args()
    ser = serial.Serial(a.port, a.baud, timeout=0.05)
    fc = FC(ser, hd=not a.sd)
    fc.variant = a.variant.encode()[:4].ljust(4, b' ')
    threading.Thread(target=fc.rx_loop, daemon=True).start()
    threading.Thread(target=fc.heartbeat_loop, daemon=True).start()
    print(f'msplab on {a.port} @ {a.baud}', flush=True)
    # the goggles ignore DisplayPort for the first ~8 s of a new session: keep drawing a marker until then
    for _ in range(9):
        fc.clear(); fc.text(0, 0, 'MSPLAB'); fc.draw(); time.sleep(1.0)
    if a.mode == 'demo':
        import osd_scroller
        os.makedirs(a.out, exist_ok=True)
        shots = []
        def on_frame(n, strs, load):
            if every and n % every == every // 2:
                p = os.path.join(a.out, f'scroller{len(shots):02d}.jpg')
                time.sleep(0.4)
                if snapshot(a.snap, p): shots.append((p, f'frame {n}, load {load:.0f}'))
        make, fps = osd_scroller.EFFECTS[a.effect]
        kw = {}
        if a.effect == 'scroller':
            kw = dict(step=a.step)
            if a.text: kw['text'] = a.text.upper()
        if a.effect == 'matrix' and a.budget:
            kw = dict(budget=a.budget)
        gen = make(**kw)
        every = max(1, int((a.fps or fps) * a.seconds / a.grab)) if a.grab and a.seconds else 0
        kw = dict(fps=a.fps or fps, seconds=a.seconds, baud=a.baud, on_frame=on_frame, safe=not a.no_safe)
        n, load, dropped, errs = osd_scroller.run(fc, gen, **kw)
        e = sorted(x * 1000 for x in errs[30:]) or [0]            # without the warm-up of the wire-time fit
        print(f'[demo] {n} frames, max load {load:.0f} of {osd_scroller.CELL_BUDGET}, {dropped} slots dropped; '
              f'DRAW vs the clock grid: median {e[len(e)//2]:+.1f} ms, 5..95 % {e[len(e)//20]:+.1f} .. {e[len(e)*19//20]:+.1f} ms, worst {e[0]:+.1f} .. {e[-1]:+.1f} ms', flush=True)
        if shots: contact_sheet(shots, os.path.join(a.out, 'scroller-sheet.jpg'), cols=2)
        return
    if a.mode == 'serve':
        while True:
            fc.clear(); fc.text(0, 0, f'MSPLAB {a.variant.upper()}')
            fc.text(2, 2, bytes(range(0x01, 0x1f))); fc.text(4, 2, 'abcdefghijklmnopqrstuvwxyz')
            fc.text(6, 2, bytes(range(0x80, 0x9e))); fc.text(8, 2, bytes(range(0xa0, 0xbe))); fc.draw()
            time.sleep(2)
            with fc.slock: print('[polls]', sorted((NAMES.get(k, k), v) for k, v in fc.seen.items()), flush=True)
    out = os.path.join(a.out, a.experiment)
    os.makedirs(out, exist_ok=True)
    items = []
    for i, step in enumerate(EXPERIMENTS[a.experiment](fc)):
        label, act, shots = step if len(step) == 3 else (*step, 1)
        act()
        time.sleep(a.dwell)
        for k in range(shots):   # several shots 0.25 s apart: shows blinking
            path = os.path.join(out, f'{i:03d}{"abcdefgh"[k] if shots > 1 else ""}.jpg')
            snapshot(a.snap, path)
            items.append((path, label + (f' [{k + 1}/{shots}]' if shots > 1 else '')))
            if k + 1 < shots: time.sleep(0.25)
        print(f'[step {i}] {label}', flush=True)
    with open(os.path.join(out, 'steps.txt'), 'w') as f:
        for path, label in items: f.write(f'{os.path.basename(path)} {label}\n')
    contact_sheet(items, os.path.join(out, 'sheet.jpg'), crop=CROPS.get(a.experiment))
    with fc.slock: print('[polls]', sorted((NAMES.get(k, k), v) for k, v in fc.seen.items()), flush=True)


if __name__ == '__main__':
    main()
