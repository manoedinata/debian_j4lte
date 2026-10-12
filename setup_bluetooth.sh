#!/bin/bash
#
# Installs a systemd unit into debootstrap-rootfs that unblocks the
# SCSC WLBT Bluetooth controller's rfkill switch at boot, so hci0
# comes up.
#
# scsc_bluez.c (drivers/misc/samsung/scsc_bt, kernel commit 5f6ce7b4)
# only calls hci_register_dev() when told to, via either its rfkill
# device ("scsc-bluez-rfkill", type bluetooth - registered in
# slsi_bluez_init(), requires CONFIG_RFKILL, kernel commit 9c1ae7a3) or
# its own power_control module parameter - normally flipped by the
# Android BT HAL, which we don't have. It starts software-blocked by
# default (rfkill_init_sw_state(..., 1)), so something still has to
# unblock it. Confirmed working by hand, same end result as writing
# directly to /sys/module/scsc_bt/parameters/power_control: hci0
# appears with firmware loaded. This is the "proper" path though -
# it's what gives bluetoothd a working rfkill control device too
# (it already looks for one at startup, see rfkill.c:rfkill_init() in
# its logs).
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOTFS_DIR="$SCRIPT_DIR/debootstrap-rootfs"

if [ ! -d "$ROOTFS_DIR" ]; then
	echo "error: missing $ROOTFS_DIR" >&2
	exit 1
fi

echo "==> installing rfkill (userspace tool)"
sudo mount --bind /dev "$ROOTFS_DIR/dev"
sudo mount -t proc proc "$ROOTFS_DIR/proc"
sudo mount -t sysfs sysfs "$ROOTFS_DIR/sys"
sudo chroot "$ROOTFS_DIR" env DEBIAN_FRONTEND=noninteractive apt-get install -y rfkill
sudo umount "$ROOTFS_DIR/dev" "$ROOTFS_DIR/proc" "$ROOTFS_DIR/sys"

echo "==> installing scsc-bt-power.service"
sudo install -d -m 755 "$ROOTFS_DIR/etc/systemd/system"
sudo tee "$ROOTFS_DIR/etc/systemd/system/scsc-bt-power.service" >/dev/null <<'EOF'
[Unit]
Description=Unblock the SCSC WLBT Bluetooth controller's rfkill switch
Before=bluetooth.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/sbin/rfkill unblock bluetooth

[Install]
WantedBy=multi-user.target
EOF
sudo install -d -m 755 "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants"
sudo ln -sf /etc/systemd/system/scsc-bt-power.service "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/scsc-bt-power.service"

echo "==> done"
