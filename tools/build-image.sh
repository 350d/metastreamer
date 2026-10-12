#!/bin/sh
# Build the SD card image in Docker: Buildroot + buildroot/ of this repo -> out/metastreamer-rpi0w.img
# The Buildroot tree and its build directory live in a docker volume ($WORK_VOLUME), so rebuilds are quick.
# First build: about an hour (toolchain, kernel). Needs docker (Docker Desktop, colima, ...).
# PUBLIC=1: an image to share, without the keys from buildroot/local (see buildroot/board/post-build.sh).
set -e
cd "$(dirname "$0")/.."
BR_VERSION=${BR_VERSION:-2026.02.3}
WORK_VOLUME=${WORK_VOLUME:-metastreamer-work}
docker build -q -t metastreamer-builder docker >/dev/null
mkdir -p out
docker run --rm -v "$WORK_VOLUME":/work -v "$PWD":/src -e BR_VERSION="$BR_VERSION" \
	-e METASTREAMER_PUBLIC="${PUBLIC:-0}" metastreamer-builder sh -ec '
	if [ ! -d /work/buildroot ]; then
		cd /work
		wget -q https://buildroot.org/downloads/buildroot-$BR_VERSION.tar.xz
		tar xf buildroot-$BR_VERSION.tar.xz && rm buildroot-$BR_VERSION.tar.xz
		mv buildroot-$BR_VERSION buildroot
	fi
	cd /work/buildroot
	make O=/work/out BR2_EXTERNAL=/src/buildroot metastreamer_rpi0w_defconfig >/dev/null
	make O=/work/out metastreamer-dirclean >/dev/null
	make O=/work/out
	cp /work/out/images/sdcard.img /src/out/metastreamer-rpi0w.img
'
ls -la out/metastreamer-rpi0w.img
