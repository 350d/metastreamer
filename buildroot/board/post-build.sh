#!/bin/sh
# After the target tree is assembled: SSH keys, permissions, the clock's lower bound, unused init scripts.
set -e
# Your own keys, from buildroot/local/ (not in git): ssh_host_ed25519_key[.pub] keeps the card's identity
# across re-flashing, authorized_keys lets you in as root. METASTREAMER_PUBLIC=1 (an image to share)
# bakes in nothing: the host key is made on the first boot, keys go to authorized_keys on the SD card.
LOCAL="$(dirname "$0")/../local"
rm -rf "${TARGET_DIR}/root/.ssh" "${TARGET_DIR}"/etc/ssh/ssh_host_*
if [ "${METASTREAMER_PUBLIC:-0}" != 1 ]; then
	if [ -f "$LOCAL/ssh_host_ed25519_key" ]; then
		install -D -m 600 "$LOCAL/ssh_host_ed25519_key" "${TARGET_DIR}/etc/ssh/ssh_host_ed25519_key"
		install -D -m 644 "$LOCAL/ssh_host_ed25519_key.pub" "${TARGET_DIR}/etc/ssh/ssh_host_ed25519_key.pub"
	fi
	if [ -f "$LOCAL/authorized_keys" ]; then
		install -D -m 600 "$LOCAL/authorized_keys" "${TARGET_DIR}/root/.ssh/authorized_keys"
		chmod 700 "${TARGET_DIR}/root/.ssh"
	fi
fi
chmod 700 "${TARGET_DIR}/root"
chmod 755 "${TARGET_DIR}"/etc/init.d/S* "${TARGET_DIR}/etc/init.d/rcS"
mkdir -p "${TARGET_DIR}/boot"
# read-only root: seedrng cannot keep its seed file; no cron jobs; modules are loaded by name
rm -f "${TARGET_DIR}/etc/init.d/S01seedrng" "${TARGET_DIR}/etc/init.d/S50crond" "${TARGET_DIR}/etc/init.d/S11modules"
# lower bound for the clock at boot (S00clock)
date -u +%s > "${TARGET_DIR}/etc/build-time"
