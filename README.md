# metastreamer

**Live video from DJI FPV goggles on any HDMI screen and in the browser, from a $15 Raspberry Pi Zero W.**

metastreamer turns a Raspberry Pi Zero W into the "phone" the goggles expect on their USB port. The goggles
stream the live H.264 video and telemetry to it; the Pi shows the video on HDMI with the hardware decoder,
serves it to browsers with about 0.2 s of latency, and lets you change the air unit's camera settings from
a web page. The whole system is a 90 MB read-only image that shows a picture about 6 s after power-on.

- **HDMI out**: hardware H.264 decode, zero-copy to a KMS plane, 1920×1080 at 50 Hz by default
- **Browser**: low-latency fMP4 over Media Source Extensions (~0.2 s), HLS for everything else
- **Snapshots**: JPEG from the hardware encoder, `/snapshot`
- **Telemetry**: link quality, goggles battery, air unit recording and storage, connected devices,
  as JSON, server-sent events and HLS metadata
- **Camera settings**: resolution, frame rate, exposure, ISO, shutter, EV, white balance, sharpness,
  noise reduction, colour profile, anti-flicker
- **Wi-Fi**: networks from one settings file on the SD card or the web page, DHCP or a static address
- **Clock**: NTP over Wi-Fi, local time zone, and it sets the goggles' clock for correct recording dates
- **Appliance-style system**: Buildroot, read-only root, hardware watchdog, survives HDMI hot-plug

> metastreamer is an independent project. It is not affiliated with, endorsed by or connected to DJI.
> DJI, DJI Goggles, DJI O3 Air Unit and DJI Fly are trademarks of SZ DJI Technology Co., Ltd.,
> used here only to say which devices the software works with.

## Supported goggles

| Goggles | USB product code | Status |
|---|---|---|
| DJI Goggles 2 | ZV900 | **Tested** (with the O3 Air Unit) |
| DJI Goggles Integra | ZV901 | Expected to work: same USB link and protocol |
| DJI Goggles 3 | ZV902 | Expected to work: same USB link and protocol |
| DJI Goggles N3 | ZV300 | Expected to work: same USB link and protocol |
| DJI FPV Goggles V1 / V2 | — | Not supported: older generation, different link |

| Air unit | Code | Camera settings |
|---|---|---|
| DJI O3 Air Unit | WM1695 | **Tested** |
| DJI O4 Air Unit | ZA530 | Untested (same commands) |
| DJI O4 Air Unit Pro | ZA5305 | Untested (same commands; 48 fps and D-Log listed) |

The goggles need no root, no modified firmware and no settings: they start streaming as soon as they see
the Pi. "Expected to work" means the goggles use the same accessory link as the tested ones; reports are welcome.

## Timings

Measured on a Raspberry Pi Zero W with Goggles 2 and an O3 Air Unit, from power-on:

| Event | Time |
|---|---|
| HDMI output, 1920×1080 at 50 Hz | ~3 s |
| Goggles registered, video on HDMI and in the browser | ~6 s |
| Wi-Fi connected, DHCP | 8.3–9.5 s |
| Wi-Fi connected, static address | ~7.8 s |
| Clock synced over NTP, goggles' clock set | ~1 s after Wi-Fi |

| Latency / recovery | Time |
|---|---|
| Browser, low-latency stream (`/live.mp4`, Safari) | ~0.2 s (estimated by eye) |
| Browser, HLS | 2–3 s |
| Goggles USB cable unplugged and plugged back in | ~5 s |
| metastreamer restarted with the cable in place | ~28 s (the goggles need 15 s off the bus, see [protocol](docs/protocol.md#restarting-a-session)) |

The same software on DietPi with systemd took 16.6 s to the first picture; the Buildroot image is what makes
it fast. The binary is about 190 KB, and the whole system uses about 90 MB of the Pi Zero's RAM.

## What you need

- **Raspberry Pi Zero W** (the image is built for it; for other Pis see [development](docs/development.md))
- microSD card, 128 MB or more
- **USB OTG adapter on the goggles' side** (USB-C plug to USB-A socket) and a USB-A to micro-USB cable
  into the Pi's **USB** port (not PWR). The goggles must be the USB host.
- mini-HDMI cable to a display or a capture card (optional)
- Power: the Pi's PWR port, or from the goggles over the same cable (keep the goggles battery charged:
  below ~25 % the Pi may brown out)

## Quick start

1. Write [`images/metastreamer-rpi0w.img.xz`](images/) to the card with Raspberry Pi Imager or balenaEtcher
   (both take the `.xz` as it is), or build your own (see [Building](#building)).
2. Open the card's boot partition (`METASTREAM`) on your computer and fill in `metastreamer.txt`:
   your country code, and your Wi-Fi under `[wifi]`, one network per line:

   ```
   country="EE"
   ...
   [wifi]
   "My home Wi-Fi"="my password"
   ```

3. Put the card in the Pi, connect HDMI and the goggles, power on the goggles and the air unit.
4. The video shows on HDMI. Open `http://metastreamer/` or the Pi's address in a browser:
   `/` is the full-screen player, `/settings` the control page.

## The control page

`http://<pi>/settings`:

- **Header**: link quality, bitrate and goggles battery in the style of the goggles' OSD; a red dot while the air unit records
- **Preview**: the live video (low-latency stream, HLS, or snapshots every 2 s, whichever the browser
  supports), full screen, links to HLS, a snapshot, `/status` and `/duml`, and the air unit's storage bar
- **Air unit camera**: every setting applies immediately
- **Settings** (gear icon): Wi-Fi networks, DHCP or static address, time zone (automatic from the browser
  or chosen by hand), and setting the goggles' clock again

## Files on the SD card

The boot partition (FAT, `METASTREAM`) holds everything you may want to change; the system itself is read-only.

| File | What it is |
|---|---|
| `metastreamer.txt` | All settings, `key="value"`: `country`, `timezone`, `timezone_auto`, `network` (`"dhcp"` or `"static"`), `address`, `gateway`, `dns`; then the Wi-Fi networks under `[wifi]`, one `"name"="password"` per line, the first one preferred |
| `authorized_keys` | Optional: SSH public keys for `root` (there is no password login) |
| `ssh_host_ed25519_key`, `.pub` | Made on the first boot: the Pi's SSH identity, kept across reboots |
| `wifi.log` | Written by the Pi: the address it got, or a report when Wi-Fi did not come up (no passwords in it) |

A wrong static address makes the Pi unreachable over Wi-Fi: set `network="dhcp"` in `metastreamer.txt` to undo it.

## HTTP endpoints

| Path | |
|---|---|
| `/` | Full-screen player |
| `/settings` | Control page |
| `/live.mp4` | Endless fMP4, one fragment per frame, for Media Source Extensions |
| `/stream.m3u8` | HLS (fMP4 segments of one GOP, telemetry in `EXT-X-DATERANGE`) |
| `/snapshot` | Current frame as JPEG |
| `/status` | JSON: link, video, telemetry, camera, HDMI |
| `/events` | The same as server-sent events, 4 per second |
| `/duml` | Every DUML message type seen from the goggles, for research |
| `/api/set?key=value` | Camera settings: `format`, `exposure`, `iso`, `shutter`, `ev`, `wb`, `sharpness`, `nr`, `color`, `flicker` |
| `/api/wifi`, `/api/net`, `/api/time`, `/api/time/sync` | Settings (GET to read, POST to change) |

There is no login: anyone on the same network can change the settings.

## Building

The image is built with [Buildroot](https://buildroot.org) in Docker:

```bash
./tools/build-image.sh
```

The result is `out/metastreamer-rpi0w.img`. The first build takes about an hour (toolchain and kernel);
later builds take a few minutes.

Your own SSH keys go to `buildroot/local/` (not in git): `authorized_keys` is baked into the image, and an
`ssh_host_ed25519_key` there keeps the card's SSH identity when you re-flash it. `PUBLIC=1 ./tools/build-image.sh`
bakes in nothing, for an image to share; that is how `images/` is built.

For working on the code without rebuilding the image, see [docs/development.md](docs/development.md).

## How it works

```
 goggles ──USB (Android accessory, serial)──► metastreamer ──► HDMI   (V4L2 H.264 decoder → KMS plane)
                                                    │
                                                    ├──► browser (fMP4 / HLS, JSON, settings page)
                                                    └──► camera settings and clock back to the goggles (DUML)
```

The Pi presents itself as an Android accessory (`18d1:2d00`) with one bulk serial function. Over it the
goggles speak DJI's LogicLink framing, with the video on its own ports and DUML messages for everything else.
metastreamer registers like the DJI Fly app, answers the heartbeats, and repackages the H.264 stream without
transcoding. The details are in [docs/protocol.md](docs/protocol.md).

## Layout

```
src/            metastreamer itself (C): USB link, fMP4/HLS and HTTP server, HDMI, telemetry, settings page
scripts/        USB gadget, session restart, Wi-Fi, NTP hook (installed to /usr/bin)
buildroot/      Buildroot external tree: defconfig, board files, init scripts, the metastreamer package
docker/         build environment for the image
contrib/        systemd units for Raspberry Pi OS / DietPi
tools/          image build, quick static build with zig
docs/           protocol notes, development
images/         the ready image for a Raspberry Pi Zero W (.img.xz)
```

## Credits

- [YStreamer](https://github.com/YLabs-FPV/YStreamer) (GPL-3.0): the USB accessory descriptor, the
  registration sequence and the camera commands were learned from it
- [N3Live](https://github.com/brendan779/N3Live) recon notes: the DUML command dictionary used to find the clock commands
- [Buildroot](https://buildroot.org), the Raspberry Pi kernel and firmware
- [Lucide](https://lucide.dev) icons (ISC)

## License

GPL-3.0-or-later, see [LICENSE](LICENSE).
