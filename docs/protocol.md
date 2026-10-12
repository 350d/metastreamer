# Protocol notes

What metastreamer speaks to the goggles, as observed on DJI Goggles 2 with an O3 Air Unit. The USB
descriptor, the registration packets and the camera commands come from
[YStreamer](https://github.com/YLabs-FPV/YStreamer); the clock commands were found with the DUML command
dictionary from [N3Live](https://github.com/brendan779/N3Live) and confirmed on the goggles.

All multi-byte numbers are little-endian.

## USB

The goggles are the USB host and look for an Android device in accessory mode. The Pi's USB controller
(`dwc2`, `dr_mode=peripheral`) presents one through configfs (`scripts/metastreamer-gadget`):

| Field | Value |
|---|---|
| idVendor / idProduct | `18d1` / `2d00` (Android Open Accessory) |
| bDeviceClass / SubClass / Protocol | `ff` / `ff` / `00` |
| bcdUSB / bcdDevice | `0200` / `0100` |
| Function | one generic serial function (`gser`), `/dev/ttyGS0` |

The goggles start talking on their own once they have enumerated the Pi. A Mac or a PC cannot do this: it
has to be a device, not a host.

`/dev/ttyGS0` must be in raw mode (`stty raw -echo`) and must not be anyone's controlling terminal: the
stream contains `0x1C`, which a terminal turns into `SIGQUIT`.

## LogicLink framing

Everything on the serial link is wrapped in 8-byte LogicLink headers:

```
55 CC  port:u16  length:u16  00 00  payload[length]
```

| Port | Direction | Content |
|---|---|---|
| `0x574A`, `0x574B` | goggles → Pi | H.264 Annex-B video; sometimes wrapped in a DUML frame (skip its 11-byte header and 2-byte CRC) |
| `0x7530` and others | goggles → Pi | DUML messages |
| `0x5749` | Pi → goggles | DUML messages |

## DUML v1

```
55  len:10 bits | version 1 (bits 10..15)  crc8  src  dst  seq:u16  flags  cmd_set  cmd_id  payload  crc16
```

- `crc8` covers the first 3 bytes: init `0x77`, polynomial `0x8C` (reflected)
- `crc16` covers everything before it: init `0x3692`, polynomial `0x8408` (reflected)
- `flags`: `0x40` request, `0x80` response; bits 5–6 set = the sender wants an acknowledgement
- An address byte is `type | index << 5`. The app (us) is `0x02`.

| Address | Role |
|---|---|
| `0x02` | the app (metastreamer) |
| `0x3C` | goggles: registration, heartbeats, clock |
| `0x28` | goggles: topic host |
| `0x01` | air unit camera |
| `0x0E`, `0x09` | radio link (link quality) |
| `0x1C`, `0xBC`, `0x48` | goggles and air unit identities |

## Registration

metastreamer registers like the DJI Fly app; the video starts right after.

1. Subscribe to `camcap_common`: to `0x28`, `00:99`, payload
   `02 02 00 00 d5 07 00 00 00 00 00 13 00`, then `0d 00`, `"camcap_common"`, `00 00 00 00`.
2. Register as the app: to `0x3C`, `00:88`, payload `17 00 00 23 00 'A' 'P' 'P' 00 00 00 00 00 02`.
3. App version: to `0x3C`, `00:88`, payload `1d 00 01 00 00 00 00 01`, `07 00`, `"1.21.1\0"`.

Then keep answering:

| Request | Answer |
|---|---|
| Heartbeat `00:88`, payload starts with `19` (about once a second) | `1a 00 00 00 00` |
| Identity `00:81` | 49 bytes: `00`, `"APP"` at offset 1, `02` at 34, the request's bytes 40..47 at 41 |
| Anything else that wants an acknowledgement | `00` |

If no heartbeat arrives for 10 s, metastreamer exits and the init script restarts it.

## Restarting a session

If metastreamer restarts while the cable stays in, the goggles keep their old session and never talk to the
new one. Dropping off the bus fixes it, but only like this (Goggles 2, five tries out of five):

1. `soft_connect` off for **15 s**
2. on for **2 s**
3. off for **3 s**
4. on

A single off period of 3, 8, 12 or 15 s does not work. `scripts/metastreamer-reconnect` does this before every
start except the first one after boot; when the cable was pulled (the tty hung up), it only waits for the
goggles, since a re-plugged cable is a fresh session anyway (about 5 s).

## Telemetry

| Message | Meaning |
|---|---|
| `06:1E` | goggles battery: payload byte 4, % |
| `09:08` from `0x0E` | video link quality: byte 0 & `0x7F`, % |
| `00:81`, `00:82` | identity: product code in the first 32 bytes (`ZV900` Goggles 2, `WM1695` O3 Air Unit, ...) |
| `02:80` | air unit recorder: byte 0 bit 7 recording; u32 total MB at 5, free MB at 9, seconds left at 17; u16 recording time at 29 |
| `02:81` | camera state push: shutter u16 at 2, ISO index at 5, EV index at 6, resolution at 13, frame rate code at 14, exposure mode at 20, WB at 23–24, ... |

`/duml` lists every message type seen with its last payload, which is the quickest way to look for more.

## Camera settings

To the air unit camera `0x01`, command set `0x02`. Reading a value is the set command + 1 with an empty
payload; the answer starts with a status byte (`00` = ok), then the value as below.

| Setting | Command | Payload |
|---|---|---|
| Format | 24 | resolution code, frame rate code, `00` |
| Exposure mode | 30 | `01` auto, `04` manual; `00` |
| Shutter | 40 | `01`, u16 32768 + speed (1/speed s), `00`; speed 0 = auto |
| ISO | 42 | index into 25, 50, 100 … 25600 (+1); `00` = auto |
| White balance | 44 | auto: `00 00`; manual: `06`, u16 kelvin / 100, `ff ff` |
| EV | 46 | index into −3.0 … +3.0 in thirds, + 7 |
| Sharpness | 56 | −3 … 3 (signed byte) |
| Colour | 66 | `00` normal, `06` D-Cinelike, `3d` D-Log |
| Noise reduction | 68 | −2 … 1 (signed byte) |
| Anti-flicker | 70 | `00` auto, `01` 60 Hz, `02` 50 Hz, `03` off |

| Resolution | 16:9 | 4:3 |
|---|---|---|
| 1080p | 10 | 98 |
| 2.7K | 45 | 95 |
| 4K | 16 | 103 |

| Frame rate | 30 | 48 | 50 | 60 | 100 | 120 |
|---|---|---|---|---|---|---|
| Code | 3 | 15 | 5 | 6 | 10 | 7 |

## Clock

The goggles keep a date for their recordings, but never set it themselves: the DJI Fly app does. The one in
our goggles was months off.

| Command | To | Payload | Answer |
|---|---|---|---|
| `00:4B` get date | `0x3C` (also answered by `0x2E`) | none | status, year u16, month, day, hour, minute, second |
| `00:4A` set date | `0x3C` | year u16, month, day, hour, minute, second | status (`00` = ok) |

metastreamer sends local time (time zone from `metastreamer.txt`) once NTP has synced, reads it back to
check, and also uses the goggles' date as a fallback system clock when it is newer than the image build time.

## Research aids

- `/duml`: every message type seen, with counts, how often the payload changed, and its first bytes
- `metastreamer --duml-probe SET:ID` (hex): for 30 s after registration, sends that request with an empty
  payload to every known address every 3 s; the answers show up in `/duml`
