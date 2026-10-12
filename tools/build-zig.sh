#!/bin/sh
# Quick static build for a Pi Zero (ARMv6, musl) with zig, no SDK needed: src/metastreamer.
# Copy it over a running card: cat src/metastreamer | ssh root@<pi> 'cat > /tmp/metastreamer'
set -e
cd "$(dirname "$0")/../src"
python3 gen_html.py web/settings.html SETTINGS_HTML settings_html.h
zig cc -target arm-linux-musleabihf -mcpu=arm1176jzf_s -O2 -Wall -Wextra -static -s \
    -o metastreamer metastreamer.c hls.c hdmi.c telem.c wifi.c conf.c
ls -la metastreamer
