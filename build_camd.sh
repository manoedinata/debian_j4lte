#!/bin/bash
#
# Builds camd (the rear-camera -> v4l2loopback bridge) and the v4l2loopback
# module for this kernel with the same gcc-6 aarch64 toolchain container as
# build_kernel.sh, and installs them into debootstrap-rootfs as a systemd
# service.
#
#   NO_INSTALL=1 ./build_camd.sh     only build; output lands in misc-files/camd/
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/camd"
KERNEL_DIR="$SCRIPT_DIR/kernel"
OUT_DIR="$SCRIPT_DIR/misc-files/camd"
ROOTFS_DIR="$SCRIPT_DIR/debootstrap-rootfs"
CONTAINER_NAME="j4-gcc6"
V4L2LOOPBACK_TAG="v0.12.7"

if [ ! -f "$SRC_DIR/camd.c" ] || [ ! -f "$KERNEL_DIR/Module.symvers" ]; then
	echo "error: need $SRC_DIR/camd.c and a built kernel ($KERNEL_DIR/Module.symvers) - run build_kernel.sh first" >&2
	exit 1
fi
if ! docker container inspect "$CONTAINER_NAME" >/dev/null 2>&1; then
	echo "error: $CONTAINER_NAME container not found - run build_kernel.sh first to create it" >&2
	exit 1
fi
if ! docker ps --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
	echo "==> starting $CONTAINER_NAME container"
	docker start "$CONTAINER_NAME" >/dev/null
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> fetching v4l2loopback $V4L2LOOPBACK_TAG (+ 3.18 patch)"
git clone -q --depth 1 --branch "$V4L2LOOPBACK_TAG" https://github.com/umlaeute/v4l2loopback.git "$WORK/v4l2loopback" 2>/dev/null
git -C "$WORK/v4l2loopback" apply "$SRC_DIR/v4l2loopback-3.18.patch"

echo "==> building v4l2loopback.ko against $KERNEL_DIR"
docker exec "$CONTAINER_NAME" rm -rf /tmp/v4l2loopback-build /tmp/camd-build
docker cp "$WORK/v4l2loopback" "$CONTAINER_NAME:/tmp/v4l2loopback-build"
docker exec -w /tmp/v4l2loopback-build "$CONTAINER_NAME" bash -c \
	'export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-; make -C /kernel M=/tmp/v4l2loopback-build modules >/dev/null'

echo "==> building camd (dynamic aarch64)"
docker cp "$SRC_DIR" "$CONTAINER_NAME:/tmp/camd-build"
docker exec -w /tmp/camd-build "$CONTAINER_NAME" aarch64-linux-gnu-gcc-6 \
	-O2 -Wall -Iinc -o camd camd.c

mkdir -p "$OUT_DIR"
docker cp "$CONTAINER_NAME:/tmp/v4l2loopback-build/v4l2loopback.ko" "$OUT_DIR/v4l2loopback.ko"
docker cp "$CONTAINER_NAME:/tmp/camd-build/camd" "$OUT_DIR/camd"
docker exec "$CONTAINER_NAME" rm -rf /tmp/v4l2loopback-build /tmp/camd-build

echo "==> built: $OUT_DIR/{camd,v4l2loopback.ko}"
if [ "${NO_INSTALL:-}" = 1 ]; then
	exit 0
fi

if [ ! -d "$ROOTFS_DIR" ]; then
	echo "error: missing $ROOTFS_DIR" >&2
	exit 1
fi

echo "==> installing into debootstrap-rootfs"
# debootstrap-rootfs is root-owned (see build_system_img.sh): a sudo prompt
# here is expected unless NOPASSWD sudo is set up.
sudo install -d -m 755 "$ROOTFS_DIR/usr/local/lib/camd" "$ROOTFS_DIR/usr/local/sbin"
sudo install -o root -g root -m 755 "$OUT_DIR/camd" "$ROOTFS_DIR/usr/local/sbin/camd"
sudo install -o root -g root -m 644 "$OUT_DIR/v4l2loopback.ko" "$ROOTFS_DIR/usr/local/lib/camd/v4l2loopback.ko"

sudo install -d -m 755 "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants"
sudo install -o root -g root -m 644 "$SRC_DIR/camd.service" "$ROOTFS_DIR/etc/systemd/system/camd.service"
sudo ln -sf /etc/systemd/system/camd.service "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/camd.service"

# FIMC-IS2 looks its firmware up under the stock Android path.
sudo install -d -m 755 "$ROOTFS_DIR/system/vendor"
sudo ln -sfn /vendor/firmware "$ROOTFS_DIR/system/vendor/firmware"

# Keep udev's v4l_id from opening every camera node at boot.
sudo install -d -m 755 "$ROOTFS_DIR/etc/udev/rules.d"
sudo install -o root -g root -m 644 "$SRC_DIR/60-persistent-v4l.rules" "$ROOTFS_DIR/etc/udev/rules.d/60-persistent-v4l.rules"

echo "==> done"
