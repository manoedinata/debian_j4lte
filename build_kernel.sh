#!/bin/bash
#
# Builds the J4 (exynos7570-j4lte) kernel using a Debian stretch Docker
# container that provides aarch64-linux-gnu-gcc-6, since that toolchain
# version is no longer apt-installable on current host distros and
# Linaro's old prebuilt binary mirrors are dead.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$SCRIPT_DIR/kernel"
CONTAINER_NAME="j4-gcc6"
JOBS="${JOBS:-$(nproc)}"
BUILD_UID="$(id -u)"
BUILD_GID="$(id -g)"

if [ ! -d "$KERNEL_DIR" ]; then
	echo "error: kernel dir not found at $KERNEL_DIR" >&2
	exit 1
fi

if ! docker container inspect "$CONTAINER_NAME" >/dev/null 2>&1; then
	echo "==> creating $CONTAINER_NAME container"
	docker run -dit --name "$CONTAINER_NAME" \
		-v "$KERNEL_DIR:/kernel" \
		debian:stretch bash
fi

if ! docker ps --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
	echo "==> starting $CONTAINER_NAME container"
	docker start "$CONTAINER_NAME" >/dev/null
fi

if ! docker exec "$CONTAINER_NAME" test -x /usr/bin/aarch64-linux-gnu-gcc-6; then
	echo "==> installing toolchain in $CONTAINER_NAME (first run only)"
	docker exec "$CONTAINER_NAME" bash -c '
		set -euo pipefail
		sed -i "s|deb.debian.org|archive.debian.org|g; s|security.debian.org|archive.debian.org|g" /etc/apt/sources.list
		sed -i "/stretch-updates/d" /etc/apt/sources.list
		sed -i "s|^deb |deb [check-valid-until=no] |" /etc/apt/sources.list
		apt-get update
		apt-get install -y \
			gcc-6-aarch64-linux-gnu binutils-aarch64-linux-gnu libc6-dev-arm64-cross \
			gcc build-essential bison flex libssl-dev libelf-dev kmod cpio rsync bc make \
			libncurses5-dev
		ln -sf aarch64-linux-gnu-gcc-6        /usr/bin/aarch64-linux-gnu-gcc
		ln -sf aarch64-linux-gnu-gcc-ar-6     /usr/bin/aarch64-linux-gnu-gcc-ar
		ln -sf aarch64-linux-gnu-gcc-nm-6     /usr/bin/aarch64-linux-gnu-gcc-nm
		ln -sf aarch64-linux-gnu-gcc-ranlib-6 /usr/bin/aarch64-linux-gnu-gcc-ranlib
	'
fi

if [ "${1:-}" = "menuconfig" ]; then
	if ! docker exec "$CONTAINER_NAME" dpkg -s libncurses5-dev >/dev/null 2>&1; then
		echo "==> installing libncurses5-dev in $CONTAINER_NAME (first run only)"
		docker exec "$CONTAINER_NAME" apt-get install -y libncurses5-dev
	fi

	echo "==> menuconfig"
	docker exec -it -u "$BUILD_UID:$BUILD_GID" -w /kernel "$CONTAINER_NAME" bash -c "
		export ARCH=arm64
		export CROSS_COMPILE=aarch64-linux-gnu-
		export ANDROID_MAJOR_VERSION=q
		[ -f .config ] || make exynos7570-j4lte_defconfig
		make menuconfig
	"
	echo "==> .config updated. Run './build_kernel.sh' to build, or"
	echo "    'make savedefconfig && cp defconfig arch/arm64/configs/exynos7570-j4lte_defconfig'"
	echo "    inside the container to persist the changes to the defconfig file."
	exit 0
fi

echo "==> building kernel (-j$JOBS)"
docker exec -u "$BUILD_UID:$BUILD_GID" -w /kernel "$CONTAINER_NAME" bash -c "
	export ARCH=arm64
	export CROSS_COMPILE=aarch64-linux-gnu-
	export ANDROID_MAJOR_VERSION=q
	[ -f .config ] || make exynos7570-j4lte_defconfig
	make -j$JOBS
"

echo "==> done: $KERNEL_DIR/arch/arm64/boot/Image"
