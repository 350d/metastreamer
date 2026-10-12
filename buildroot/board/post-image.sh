#!/bin/bash
# SD card image: boot (FAT: firmware, kernel, dtb, overlays, wifi.txt, metastreamer.txt) + rootfs (squashfs, read-only)
set -e
BOARD_DIR="$(dirname $0)"
GENIMAGE_CFG="${BINARIES_DIR}/genimage.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"
cp "${BOARD_DIR}/wifi.txt" "${BINARIES_DIR}/wifi.txt"
cp "${BOARD_DIR}/metastreamer.txt" "${BINARIES_DIR}/metastreamer.txt"
FILES=()
for i in "${BINARIES_DIR}"/*.dtb "${BINARIES_DIR}"/rpi-firmware/*; do FILES+=( "${i#${BINARIES_DIR}/}" ); done
FILES+=( "zImage" "wifi.txt" "metastreamer.txt" )
BOOT_FILES=$(printf '\\t\\t\\t"%s",\\n' "${FILES[@]}")
sed "s|#BOOT_FILES#|${BOOT_FILES}|" "${BOARD_DIR}/genimage.cfg.in" > "${GENIMAGE_CFG}"
trap 'rm -rf "${ROOTPATH_TMP}"' EXIT
ROOTPATH_TMP="$(mktemp -d)"
rm -rf "${GENIMAGE_TMP}"
genimage --rootpath "${ROOTPATH_TMP}" --tmppath "${GENIMAGE_TMP}" --inputpath "${BINARIES_DIR}" \
	--outputpath "${BINARIES_DIR}" --config "${GENIMAGE_CFG}"
