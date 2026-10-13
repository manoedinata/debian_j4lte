#!/bin/bash
#
# Builds and installs fbkeyboard into debootstrap-rootfs: an on-screen
# keyboard for the Linux framebuffer console. The J4 has no hardware
# keyboard, so without this there's no way to type at a bare console
# (before any desktop environment is up) - fbkeyboard reads touch
# events from /dev/input/eventN and injects keystrokes via uinput,
# which this kernel already has enabled (CONFIG_INPUT_UINPUT,
# CONFIG_TOUCHSCREEN_MELFAS_MMS438, CONFIG_FRAMEBUFFER_CONSOLE).
#
# Installs fbkeyboard.service so it starts automatically at boot and
# stays up across VT switches (Restart=always). It's pinned to the
# touchscreen's udev by-path symlink
# (/dev/input/by-path/platform-13840000.i2c-event) rather than an
# eventN number, since that's the Melfas touchscreen's fixed i2c
# controller address in the devicetree - stable across boots,
# unlike event node numbering which depends on driver probe order.
# Ordering the unit on the by-path device itself (systemd .device
# unit) turned out to hang the start job on this system - udev marks
# it "loaded" but never "active" even though the symlink already
# exists - so this just relies on Restart=always to retry until the
# node is there instead.
#
# Builds from the fbkeyboard git submodule (manoedinata/fbkeyboard, a
# fork of bakonyiferenc/fbkeyboard), not vanilla upstream. Upstream
# pushes frames to the screen via lseek()+write() on /dev/fb0, but the
# J4's decon_fb char device doesn't implement read/write at all
# (confirmed empirically: write() returns 0 and has no effect) - it
# only supports mmap+ioctl, which is what DRM/fbdev-over-DRM shims
# generally expose. The fork's "Draw via mmap instead of write(), force
# opaque alpha" commit switches show_fbkeyboard() to mmap /dev/fb0 once
# and memcpy into it instead, and also forces the alpha byte to 0xff on
# every drawn pixel (the panel's fb_var_screeninfo reports a real 8-bit
# transp channel, i.e. the decon layer is ARGB8888 and composites
# per-pixel alpha - upstream never sets it, so without this keys drawn
# with alpha=0 would be invisible even once the mmap fix makes the
# writes actually land).
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOTFS_DIR="$SCRIPT_DIR/debootstrap-rootfs"
FBKB_SRC_DIR="$SCRIPT_DIR/fbkeyboard"

if [ ! -d "$ROOTFS_DIR" ]; then
	echo "error: missing $ROOTFS_DIR" >&2
	exit 1
fi

if [ ! -f "$FBKB_SRC_DIR/fbkeyboard.c" ]; then
	echo "error: missing $FBKB_SRC_DIR (submodule not checked out - run: git submodule update --init fbkeyboard)" >&2
	exit 1
fi

echo "==> installing build deps"
sudo mount --bind /dev "$ROOTFS_DIR/dev"
sudo mount -t proc proc "$ROOTFS_DIR/proc"
sudo mount -t sysfs sysfs "$ROOTFS_DIR/sys"
sudo chroot "$ROOTFS_DIR" env DEBIAN_FRONTEND=noninteractive apt-get install -y \
	build-essential libfreetype6-dev pkg-config fonts-dejavu-core

echo "==> copying fbkeyboard source ($(cd "$FBKB_SRC_DIR" && git rev-parse --short HEAD))"
sudo rm -rf "$ROOTFS_DIR/usr/local/src/fbkeyboard"
sudo mkdir -p "$ROOTFS_DIR/usr/local/src/fbkeyboard"
sudo rsync -a --exclude='.git' "$FBKB_SRC_DIR/" "$ROOTFS_DIR/usr/local/src/fbkeyboard/"

echo "==> building fbkeyboard"
sudo chroot "$ROOTFS_DIR" make -C /usr/local/src/fbkeyboard
sudo install -m 755 "$ROOTFS_DIR/usr/local/src/fbkeyboard/fbkeyboard" "$ROOTFS_DIR/usr/local/bin/fbkeyboard"

sudo umount "$ROOTFS_DIR/dev" "$ROOTFS_DIR/proc" "$ROOTFS_DIR/sys"

echo "==> installing fbkeyboard.service"
sudo install -d -m 755 "$ROOTFS_DIR/etc/systemd/system"
sudo tee "$ROOTFS_DIR/etc/systemd/system/fbkeyboard.service" >/dev/null <<'EOF'
[Unit]
Description=On-screen keyboard for the framebuffer console
After=systemd-udevd.service

[Service]
ExecStart=/usr/local/bin/fbkeyboard -d /dev/input/by-path/platform-13840000.i2c-event -f /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
sudo install -d -m 755 "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants"
sudo ln -sf /etc/systemd/system/fbkeyboard.service "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/fbkeyboard.service"

echo "==> done"
