#!/bin/bash
#
# Packs debootstrap-rootfs into an ext4 system.img sized for the J4's
# SYSTEM partition, using a root Docker container (mke2fs -d needs to
# read root-owned files like /root that this host user can't). ext4
# feature flags/mount opts are pinned to match the stock SYSTEM
# partition's superblock (see notes.txt) so the device's downstream
# kernel can actually mount it.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOTFS_DIR="$SCRIPT_DIR/debootstrap-rootfs"
OUT_DIR="$SCRIPT_DIR/misc-files"
OUT_IMG="${OUT_IMG:-system.img}"
CONTAINER_NAME="j4-e2fsprogs"

# SYSTEM partition size from notes.txt (blockdev --getsize64): 2625634304 bytes.
# (SIZE/LABEL/OUT_IMG can be overridden to build an image for another partition, e.g. USERDATA.)
SIZE="${SIZE:-2504M}"
LABEL="${LABEL:-system}"
# Stock SYSTEM's feature set (see notes.txt) lacks resize_inode, has_journal,
# flex_bg, metadata_csum and 64bit (all defaults for ext4 in mke2fs.conf), but
# has meta_bg and uninit_bg (not defaults). resize_inode and meta_bg are
# mutually exclusive, so the defaults must be negated rather than just adding
# meta_bg on top.
FEATURES="^resize_inode,^has_journal,^flex_bg,^metadata_csum,^64bit,meta_bg,uninit_bg"

if [ ! -d "$ROOTFS_DIR" ]; then
	echo "error: $ROOTFS_DIR not found" >&2
	exit 1
fi
mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR/$OUT_IMG"

if ! docker container inspect "$CONTAINER_NAME" >/dev/null 2>&1; then
	echo "==> creating $CONTAINER_NAME container"
	docker run -dit --name "$CONTAINER_NAME" \
		-v "$ROOTFS_DIR:/rootfs:ro" \
		-v "$OUT_DIR:/out" \
		debian:bookworm-slim bash
fi

if ! docker ps --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
	echo "==> starting $CONTAINER_NAME container"
	docker start "$CONTAINER_NAME" >/dev/null
fi

if ! docker exec "$CONTAINER_NAME" test -x /sbin/mke2fs; then
	echo "==> installing e2fsprogs in $CONTAINER_NAME (first run only)"
	docker exec "$CONTAINER_NAME" bash -c 'apt-get update && apt-get install -y e2fsprogs'
fi

echo "==> building $OUT_IMG ($SIZE) from $ROOTFS_DIR"
docker exec "$CONTAINER_NAME" mke2fs -F -t ext4 -b 4096 -I 256 -m 0 \
	-L "$LABEL" -O "$FEATURES" \
	-d /rootfs /out/$OUT_IMG "$SIZE"

echo "==> matching default mount opts / fsck interval to stock SYSTEM"
docker exec "$CONTAINER_NAME" tune2fs -o user_xattr,acl -c 0 -i 0 /out/$OUT_IMG

docker exec "$CONTAINER_NAME" chown "$(id -u)":"$(id -g)" /out/$OUT_IMG

echo "==> done: $OUT_DIR/$OUT_IMG"
ls -lh "$OUT_DIR/$OUT_IMG"
