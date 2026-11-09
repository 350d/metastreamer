# Development

## Quick builds without the image

`tools/build-zig.sh` makes a static ARMv6 binary with [zig](https://ziglang.org) (no SDK, no Docker):

```bash
./tools/build-zig.sh
```

Try it on a running card without touching the read-only system: stop the running copy and start yours from
`/tmp`. It takes the USB gadget over from the running copy:

```bash
cat src/metastreamer | ssh root@metastreamer 'cat > /tmp/metastreamer && chmod +x /tmp/metastreamer'
```

```bash
ssh root@metastreamer 'kill $(ps | grep "[S]10metastreamer start" | cut -c1-6) $(pidof metastreamer); /tmp/metastreamer'
```

A reboot brings back the version in the image.

## Options

```
metastreamer [--http PORT] [--no-hdmi] [--hdmi-mode WxH[@HZ]|auto] [--duml-probe SET:ID] [--duml-console]
metastreamer --wifi-conf metastreamer.txt wpa_supplicant.conf
```

- `--no-hdmi`: no display; the decoder still runs for `/snapshot`
- `--hdmi-mode`: default 1920×1080 at 60 Hz (else 50); `auto` takes the display's preferred mode
  (capture cards often prefer 720p)
- `--duml-probe`, `--duml-console`: see [protocol.md](protocol.md#research-aids)
- `--wifi-conf`: used at boot by `metastreamer-wifi` to turn the `[wifi]` section into a `wpa_supplicant` config

## Source

| File | |
|---|---|
| `src/metastreamer.c` | LogicLink/DUML, registration, camera commands, clock, main loop |
| `src/usb.c` | the USB gadget: configfs, FunctionFS, endpoint threads, session restart |
| `src/hls.c` | H.264 → fMP4 (per-frame stream and HLS), HTTP server, pages, API |
| `src/hdmi.c` | V4L2 hardware decoder, KMS output, JPEG snapshots |
| `src/screen.c` | the status screen (HDMI under the video, `/snapshot` without video); `src/jpeg.c` encodes it |
| `src/splash.c` | `metastreamer --splash`: the same screen as the boot screen on the console framebuffer, started by `rcS` |
| `src/ui_assets.h` | its font, icons and animations, made from `assets/` by `tools/gen-ui.py` (any TTF/OTF; needs Pillow, fontTools, numpy; kept in git) |
| `src/telem.c` | telemetry from DUML, `/duml` monitor |
| `src/wifi.c` | Wi-Fi: `wpa_supplicant` config, `/api/wifi`, `/api/net` |
| `src/conf.c` | `metastreamer.txt`: all settings, read and written in one place |
| `src/web/settings.html` | the control page (compiled into the binary by `gen_html.py`) |

## The image

- Buildroot 2026.02 LTS, musl, BusyBox init, Raspberry Pi kernel 6.12 (`bcmrpi` + `buildroot/board/linux.fragment`:
  no tracing, which alone took 1.7 s of the boot; the USB controller driver `dwc2` as a module, loaded in the
  background by `/etc/inittab`, so the display does not wait the ~0.5 s the firmware takes to power USB up).
  HDMI comes up 0.45 s into the kernel, the goggles see the Pi at 1.75 s (`dmesg`)
- Root file system: squashfs (LZ4), read-only; `/run` and `/tmp` in RAM; the boot partition is mounted read-only
  and remounted for a moment when the control page saves something
- Init scripts in `buildroot/rootfs/etc/init.d`: `S02metastreamer` (right after syslogd) starts the gadget and keeps metastreamer
  running, `S30wifi` brings Wi-Fi up in the background, so the HDMI picture never waits for the network
- `rcS` starts the boot screen (`metastreamer --splash`); the scripts' own output, with the uptime each one
  started at, goes to `/run/boot.log`. Measuring the kernel: `initcall_debug` in `cmdline.txt`
- One HDMI signal from power-on: the firmware leaves HDMI alone (`hdmi_ignore_hotplug=1`), the console starts in
  the mode metastreamer uses (`video=` in `cmdline.txt`), and metastreamer takes it over without a modeset
- `/run/wifi.state`: `ok`, or `error` and why Wi-Fi is not connected (shown on the status screen)

Useful on a running card: `logread` is not there, use `/var/log/messages`, `/run/boot.log`,
`/run/wpa_supplicant.log` and `/run/wifi.out`.

## Raspberry Pi OS / DietPi

metastreamer also runs on a full distribution (tested on DietPi on a Pi Zero W, slower to boot):

- `dtoverlay=dwc2,dr_mode=peripheral` in `config.txt`; `gpu_mem=128` and `dtoverlay=vc4-kms-v3d,cma-128` for the
  decoder (DietPi defaults to 16 MB and blacklists `bcm2835_codec`)
- copy `src/metastreamer` and `scripts/*` to `/usr/local/bin`
- install `contrib/systemd/*.service`; load `i2c_bcm2835` before `vc4`, or the display waits up to 20 s for DDC
