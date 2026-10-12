#!/bin/bash
#
# Copies freshly built kernel/dtb and the initramfs tree into the
# AIK-Linux-mirror working dirs, ready for ./AIK-Linux-mirror/repackimg.sh.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_IMAGE="$SCRIPT_DIR/kernel/arch/arm64/boot/Image"
KERNEL_DTB="$SCRIPT_DIR/kernel/arch/arm64/boot/dtb-exynos.img"
INITRAMFS_DIR="$SCRIPT_DIR/initramfs"
SPLIT_DIR="$SCRIPT_DIR/AIK-Linux-mirror/split_img"
RAMDISK_DIR="$SCRIPT_DIR/AIK-Linux-mirror/ramdisk"

for f in "$KERNEL_IMAGE" "$KERNEL_DTB"; do
	if [ ! -f "$f" ]; then
		echo "error: missing $f (run build_kernel.sh first)" >&2
		exit 1
	fi
done
if [ ! -d "$INITRAMFS_DIR" ] || [ ! -d "$SPLIT_DIR" ] || [ ! -d "$RAMDISK_DIR" ]; then
	echo "error: AIK-Linux-mirror or initramfs dir not found under $SCRIPT_DIR" >&2
	exit 1
fi

echo "==> kernel: $KERNEL_IMAGE -> split_img/boot_stock.img-kernel"
cp -f "$KERNEL_IMAGE" "$SPLIT_DIR/boot_stock.img-kernel"

echo "==> dtb: $KERNEL_DTB -> split_img/boot_stock.img-dt"
cp -f "$KERNEL_DTB" "$SPLIT_DIR/boot_stock.img-dt"

echo "==> initramfs: $INITRAMFS_DIR/ -> ramdisk/"
rsync -a --delete "$INITRAMFS_DIR/" "$RAMDISK_DIR/"

echo "==> done. Run AIK-Linux-mirror/repackimg.sh to build image-new.img"
