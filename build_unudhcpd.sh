#!/bin/bash
#
# Cross-compiles unudhcpd (static DHCP server for a single USB-tethered
# client, from postmarketOS) using the same gcc-6-aarch64-linux-gnu
# toolchain container as build_kernel.sh, and installs the resulting
# static aarch64 binary into both:
#   - debootstrap-rootfs/usr/local/sbin/unudhcpd (the Debian rootfs)
#   - initramfs/bin/unudhcpd                     (the initramfs)
#
# It's built -static with no package-manager install step (meson/ninja
# aren't in the build container, and the source is two small .c files
# with no deps beyond libc) so the exact same binary drops into either
# tree, including the initramfs, which has no dynamic linker at all.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/unudhcpd"
CONTAINER_NAME="j4-gcc6"
ROOTFS_DIR="$SCRIPT_DIR/debootstrap-rootfs"
INITRAMFS_DIR="$SCRIPT_DIR/initramfs"

if [ ! -f "$SRC_DIR/main.c" ]; then
	echo "error: $SRC_DIR looks empty (run 'git submodule update --init unudhcpd')" >&2
	exit 1
fi
for d in "$ROOTFS_DIR" "$INITRAMFS_DIR"; do
	if [ ! -d "$d" ]; then
		echo "error: missing $d" >&2
		exit 1
	fi
done

if ! docker container inspect "$CONTAINER_NAME" >/dev/null 2>&1; then
	echo "error: $CONTAINER_NAME container not found - run build_kernel.sh first to create it" >&2
	exit 1
fi
if ! docker ps --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
	echo "==> starting $CONTAINER_NAME container"
	docker start "$CONTAINER_NAME" >/dev/null
fi

VERSION="$(cd "$SRC_DIR" && git describe --tags --always 2>/dev/null || echo 0.1)"

echo "==> building unudhcpd $VERSION (static aarch64)"
docker exec "$CONTAINER_NAME" rm -rf /tmp/unudhcpd-build
docker cp "$SRC_DIR" "$CONTAINER_NAME:/tmp/unudhcpd-build"
docker exec -w /tmp/unudhcpd-build "$CONTAINER_NAME" aarch64-linux-gnu-gcc-6 \
	-O2 -Wall -static -DVERSION="\"$VERSION\"" \
	-o unudhcpd main.c server.c

mkdir -p "$SCRIPT_DIR/misc-files"
docker cp "$CONTAINER_NAME:/tmp/unudhcpd-build/unudhcpd" "$SCRIPT_DIR/misc-files/unudhcpd"
docker exec "$CONTAINER_NAME" rm -rf /tmp/unudhcpd-build

echo "==> installing into debootstrap-rootfs (/usr/local/sbin/unudhcpd)"
# A sudo prompt here is expected: debootstrap-rootfs is root-owned (see
# build_system_img.sh), so copying into it needs a real privilege escalation.
sudo install -o root -g root -m 755 "$SCRIPT_DIR/misc-files/unudhcpd" "$ROOTFS_DIR/usr/local/sbin/unudhcpd"

echo "==> installing into initramfs (/bin/unudhcpd)"
install -m 755 "$SCRIPT_DIR/misc-files/unudhcpd" "$INITRAMFS_DIR/bin/unudhcpd"

# getprotobyname("udp") in server.c reads /etc/protocols at runtime; the
# initramfs has no /etc at all otherwise, so ship the one line it needs
# (debootstrap-rootfs already has a full /etc/protocols from netbase).
mkdir -p "$INITRAMFS_DIR/etc"
if [ ! -f "$INITRAMFS_DIR/etc/protocols" ]; then
	printf 'udp\t17\tUDP\t\t# user datagram protocol\n' >"$INITRAMFS_DIR/etc/protocols"
fi

echo "==> done: $SCRIPT_DIR/misc-files/unudhcpd"
