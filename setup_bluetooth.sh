#!/bin/bash
#
# Installs a systemd unit into debootstrap-rootfs that powers on the
# SCSC WLBT Bluetooth controller at boot.
#
# scsc_bluez.c (drivers/misc/samsung/scsc_bt, kernel commit 5f6ce7b4)
# only calls hci_register_dev() when told to, via either an rfkill
# unblock (CONFIG_RFKILL isn't enabled in this kernel, so that path
# doesn't exist) or its own power_control module parameter - normally
# flipped by the Android BT HAL, which we don't have. Confirmed working
# by hand: `echo 1 > /sys/module/scsc_bt/parameters/power_control` makes
# hci0 appear with firmware loaded. This just does that at boot instead.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOTFS_DIR="$SCRIPT_DIR/debootstrap-rootfs"

if [ ! -d "$ROOTFS_DIR" ]; then
	echo "error: missing $ROOTFS_DIR" >&2
	exit 1
fi

echo "==> installing scsc-bt-power.service"
sudo install -d -m 755 "$ROOTFS_DIR/etc/systemd/system"
sudo tee "$ROOTFS_DIR/etc/systemd/system/scsc-bt-power.service" >/dev/null <<'EOF'
[Unit]
Description=Power on the SCSC WLBT Bluetooth controller
Before=bluetooth.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'echo 1 > /sys/module/scsc_bt/parameters/power_control'

[Install]
WantedBy=multi-user.target
EOF
sudo install -d -m 755 "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants"
sudo ln -sf /etc/systemd/system/scsc-bt-power.service "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/scsc-bt-power.service"

echo "==> done"
