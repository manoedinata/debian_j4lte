#!/bin/bash
#
# Installs samsungipcd (built by build_samsungipcd.sh) into
# debootstrap-rootfs along with its runtime deps (ModemManager, the
# udev rule it needs, and a systemd unit).
#
# NOT auto-enabled at boot: libsamsung-ipc has no device profile for
# the J4's modem (ss310ap/ss305ap) - it will fall back to "herolte"
# (Galaxy S7, Shannon333), which may or may not actually boot the J4's
# CP firmware correctly. This needs testing on real hardware first.
# See memory/mobile_data_samsungipcd.md. Start it by hand once built:
#   systemctl start samsungipcd
# and only `systemctl enable` it once that's been confirmed to work.
#
# The CP firmware partition: herolte's boot path
# (samsung-ipc/devices/herolte/herolte.c: modem_image_devices[]) reads
# raw CP firmware from /dev/disk/by-partlabel/RADIO first. Confirmed
# via `ls /dev/block/by-name/` on stock Android (toolbox shell) that
# RADIO is a real GPT partition name on this device (mmcblk0p12, not
# an Android-only alias) - standard udev (60-persistent-storage.rules,
# via blkid's GPT PARTLABEL) should create that same symlink under
# Debian with no extra setup. Worth a quick `ls /dev/disk/by-partlabel/`
# check once booted, but no code change is expected to be needed here.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOTFS_DIR="$SCRIPT_DIR/debootstrap-rootfs"
BUILD_DIR="$SCRIPT_DIR/misc-files/samsungipcd"

if [ ! -d "$ROOTFS_DIR" ]; then
	echo "error: missing $ROOTFS_DIR" >&2
	exit 1
fi
if [ ! -x "$BUILD_DIR/samsungipcd" ]; then
	echo "error: $BUILD_DIR/samsungipcd not found - run ./build_samsungipcd.sh first" >&2
	exit 1
fi

echo "==> installing ModemManager (rootfs)"
sudo mount --bind /dev "$ROOTFS_DIR/dev"
sudo mount -t proc proc "$ROOTFS_DIR/proc"
sudo mount -t sysfs sysfs "$ROOTFS_DIR/sys"
sudo chroot "$ROOTFS_DIR" env DEBIAN_FRONTEND=noninteractive apt-get install -y modemmanager
sudo umount "$ROOTFS_DIR/dev" "$ROOTFS_DIR/proc" "$ROOTFS_DIR/sys"

echo "==> installing samsungipcd, libsamsung-ipc.so, ppp-hook.so"
sudo install -D -o root -g root -m 0755 "$BUILD_DIR/samsungipcd" "$ROOTFS_DIR/usr/sbin/samsungipcd"
sudo install -D -o root -g root -m 0755 "$BUILD_DIR/ppp-hook.so" "$ROOTFS_DIR/usr/share/samsungipcd/ppp-hook.so"
sudo install -D -o root -g root -m 0755 "$BUILD_DIR/libsamsung-ipc.so.0.0.0" "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/libsamsung-ipc.so.0.0.0"
sudo ln -sf libsamsung-ipc.so.0.0.0 "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/libsamsung-ipc.so.0"
sudo ln -sf libsamsung-ipc.so.0.0.0 "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/libsamsung-ipc.so"
sudo chroot "$ROOTFS_DIR" ldconfig

echo "==> installing the ttyGSM udev rule"
sudo install -d -m 755 "$ROOTFS_DIR/etc/udev/rules.d"
sudo tee "$ROOTFS_DIR/etc/udev/rules.d/80-samsungipcd.rules" >/dev/null <<'EOF'
# samsungipcd exposes the modem as two CUSE char devices (see
# samsungipcd.service); tag them so ModemManager picks them up.
KERNEL=="ttyGSM0", ENV{ID_MM_DEVICE_PROCESS}="1"
KERNEL=="ttyGSM1", ENV{ID_MM_DEVICE_PROCESS}="1"
EOF

echo "==> installing samsungipcd.service (not enabled - see header comment)"
sudo install -d -m 755 "$ROOTFS_DIR/etc/systemd/system"
sudo tee "$ROOTFS_DIR/etc/systemd/system/samsungipcd.service" >/dev/null <<'EOF'
[Unit]
Description=Samsung IPC modem AT command proxy (ss310ap/herolte profile)

[Service]
ExecStart=/usr/sbin/samsungipcd cuse:/dev/ttyGSM0 cuse:/dev/ttyGSM1
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF

echo "==> telling NetworkManager to leave rmnet0 alone"
# samsungipcd talks to rmnet0 directly via a raw AF_PACKET/AF_INET
# socket (see INTERFACE_NAME in samsung-ipc.c) - it's not a normal IP
# netdev NetworkManager should try to configure/DHCP on. The actual
# user-visible data connection comes up as a ppp0 interface once
# ModemManager dials, which NetworkManager should manage normally.
sudo install -d -m 755 "$ROOTFS_DIR/etc/NetworkManager/conf.d"
sudo tee "$ROOTFS_DIR/etc/NetworkManager/conf.d/10-unmanaged-rmnet0.conf" >/dev/null <<'EOF'
[keyfile]
unmanaged-devices=interface-name:rmnet0
EOF

echo "==> done"
echo
echo "Still open before this can work end-to-end (see memory/mobile_data_samsungipcd.md):"
echo "  1. Test 'systemctl start samsungipcd' and watch dmesg/journalctl - the herolte"
echo "     CP boot sequence is for the wrong chip (Shannon333/S7, not Shannon310/J4) and"
echo "     may fail or need a dedicated ss310ap device port in libsamsung-ipc."
echo "  2. Confirm how to get ModemManager/pppd to actually load ppp-hook.so when"
echo "     dialing (*99#) - this isn't wired up by anything in this script yet."
