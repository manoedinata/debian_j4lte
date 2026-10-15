#!/bin/bash
#
# Cross-compiles samsungipcd (git submodule - carries local fixes
# committed directly on top of upstream, see its own `git log`) and
# its dependency libsamsung-ipc (git submodule: the manoedinata fork,
# branch shannon-lte-data, which is upstream commit 7ce1371f - the last
# commit with herolte (Shannon) device support before an unrelated
# libcurl build dependency was added upstream - plus the messages that
# Shannon modems need for packet data) using the same
# gcc-6-aarch64-linux-gnu toolchain container as build_kernel.sh, and
# installs the results into debootstrap-rootfs.
#
# libsamsung-ipc has no device profile for the J4's modem
# (ss310ap/ss305ap). Its board-detection fallback (see
# samsung-ipc/ipc.c:ipc_device_detect()) means it will load the
# "herolte" (Galaxy S7, Shannon333) profile for any unrecognized
# board, since herolte is the only table entry with board_name=NULL.
# That may or may not actually boot the J4's CP firmware correctly -
# this needs testing on real hardware. See
# memory/mobile_data_samsungipcd.md.
#
# ppp-hook.so (the pppd plugin letting pppd hand PPP framing to
# samsungipcd over /dev/ppp instead of a real tty) needs pppd's
# internal headers, which Debian does not ship as a -dev package.
# This script pulls them out of the *exact* ppp source version
# installed in debootstrap-rootfs via `apt-get source` inside a
# chroot, so the plugin ABI (struct layouts, PPPD_VERSION) matches.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IPC_SRC_DIR="$SCRIPT_DIR/libsamsung-ipc"
DAEMON_SRC_DIR="$SCRIPT_DIR/samsungipcd"
ROOTFS_DIR="$SCRIPT_DIR/debootstrap-rootfs"
CONTAINER_NAME="j4-gcc6"
BUILD_DIR_IPC="/tmp/libsamsung-ipc-build"
BUILD_DIR_DAEMON="/tmp/samsungipcd-build"
SYSROOT="/tmp/samsungipcd-sysroot"
PPPD_HEADER_CACHE="$SCRIPT_DIR/misc-files/samsungipcd/pppd-headers"

if [ ! -f "$IPC_SRC_DIR/configure.ac" ] || [ ! -f "$DAEMON_SRC_DIR/server.c" ]; then
	echo "error: submodules not checked out - run 'git submodule update --init'" >&2
	exit 1
fi
if [ ! -d "$ROOTFS_DIR" ]; then
	echo "error: missing $ROOTFS_DIR" >&2
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

echo "==> installing host-side build deps in $CONTAINER_NAME (first run only)"
docker exec "$CONTAINER_NAME" bash -c '
	set -euo pipefail
	need=0
	for p in autoconf automake libtool pkg-config; do
		dpkg -s "$p" >/dev/null 2>&1 || need=1
	done
	if [ "$need" = 1 ]; then
		apt-get install -y autoconf automake libtool pkg-config
	fi
'

# OpenSSL: link against the *target* rootfs's actual libssl (bullseye,
# 1.1), not some arm64 cross package installed in this (Debian
# stretch) container - stretch only has libssl1.0-dev:arm64 (OpenSSL
# 1.0.2), which produced a libsamsung-ipc.so that failed to load on
# the real device ("libssl.so.1.0.2: cannot open shared object
# file") since bullseye only ships libssl.so.1.1. Pull the dev files
# straight out of debootstrap-rootfs instead, so the ABI always
# matches whatever's actually going to run this.
OPENSSL_SYSROOT="/tmp/openssl-sysroot"
if ! sudo chroot "$ROOTFS_DIR" dpkg -s libssl-dev >/dev/null 2>&1; then
	echo "==> installing libssl-dev in debootstrap-rootfs (first run only)"
	sudo mount --bind /dev "$ROOTFS_DIR/dev"
	sudo mount -t proc proc "$ROOTFS_DIR/proc"
	sudo mount -t sysfs sysfs "$ROOTFS_DIR/sys"
	sudo chroot "$ROOTFS_DIR" env DEBIAN_FRONTEND=noninteractive apt-get install -y libssl-dev
	sudo umount "$ROOTFS_DIR/dev" "$ROOTFS_DIR/proc" "$ROOTFS_DIR/sys"
fi
echo "==> staging the rootfs's OpenSSL (dev files) as a cross sysroot"
docker exec "$CONTAINER_NAME" rm -rf "$OPENSSL_SYSROOT"
docker exec "$CONTAINER_NAME" mkdir -p "$OPENSSL_SYSROOT/usr/include" "$OPENSSL_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig"
docker cp "$ROOTFS_DIR/usr/include/openssl" "$CONTAINER_NAME:$OPENSSL_SYSROOT/usr/include/openssl"
for f in libssl.so libssl.so.1.1 libcrypto.so libcrypto.so.1.1; do
	docker cp "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/$f" "$CONTAINER_NAME:$OPENSSL_SYSROOT/usr/lib/aarch64-linux-gnu/$f"
done
for f in openssl.pc libssl.pc libcrypto.pc; do
	docker cp "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/pkgconfig/$f" "$CONTAINER_NAME:$OPENSSL_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig/$f"
done

# Forces the "herolte" device profile at compile time
# (-DIPC_DEVICE_NAME), since ipc_device_detect() (samsung-ipc/ipc.c)
# only has a fallback for devices with no matching name/board_name at
# *runtime*, via /proc/cpuinfo's "Hardware:" line - and arm64 kernels
# don't print that field at all (it's an arch/arm 32-bit relic), so
# detection would otherwise always return -1 and samsungipcd would
# fail with "modem init failed" before ever touching the modem driver.
# Confirmed on-device: this kernel's /proc/cpuinfo has no Hardware
# line (uname -r 3.18.140).
echo "==> cross-building libsamsung-ipc ($(cd "$IPC_SRC_DIR" && git rev-parse --short HEAD))"
docker exec "$CONTAINER_NAME" rm -rf "$BUILD_DIR_IPC" "$SYSROOT"
docker cp "$IPC_SRC_DIR" "$CONTAINER_NAME:$BUILD_DIR_IPC"
# Written to a script file (rather than passed inline as a `bash -c`
# string) specifically so the CFLAGS quoting below survives intact:
# it needs a literal `'"herolte"'` (single-quoted double-quoted
# string) to still be there after autoconf's generated Makefile
# re-interprets CFLAGS through another shell layer at `make` time -
# nesting that through an extra shell-quoted `bash -c "..."` argument
# here would need the quotes escaped twice over. A heredoc with a
# quoted delimiter passes its body through completely literally.
IPC_BUILD_SCRIPT="$(mktemp)"
cat > "$IPC_BUILD_SCRIPT" <<'SCRIPT_EOF'
set -euo pipefail
rm -rf .git
./autogen.sh
./configure --host=aarch64-linux-gnu CC=aarch64-linux-gnu-gcc-6 \
	--prefix=/usr --sysconfdir=/etc \
	CFLAGS="-Dloff_t=off_t -DIPC_DEVICE_NAME='\"herolte\"'" \
	LDFLAGS="-L$OPENSSL_SYSROOT/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,$OPENSSL_SYSROOT/usr/lib/aarch64-linux-gnu"
make -j"$(nproc)"
make install DESTDIR="$SYSROOT"
SCRIPT_EOF
docker cp "$IPC_BUILD_SCRIPT" "$CONTAINER_NAME:$BUILD_DIR_IPC/.build.sh"
rm -f "$IPC_BUILD_SCRIPT"
docker exec -w "$BUILD_DIR_IPC" \
	-e "OPENSSL_SYSROOT=$OPENSSL_SYSROOT" -e "SYSROOT=$SYSROOT" \
	-e "PKG_CONFIG_LIBDIR=$OPENSSL_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig" \
	-e "PKG_CONFIG_SYSROOT_DIR=$OPENSSL_SYSROOT" \
	"$CONTAINER_NAME" bash .build.sh

echo "==> fetching pppd headers matching debootstrap-rootfs's installed ppp version"
if [ -f "$PPPD_HEADER_CACHE/pppd/pppd.h" ]; then
	echo "    (cached at $PPPD_HEADER_CACHE)"
else
	PPP_VERSION="$(sudo chroot "$ROOTFS_DIR" dpkg-query -W -f='${Version}' ppp)"
	echo "    rootfs has ppp $PPP_VERSION installed - fetching matching source"
	sudo mount --bind /dev "$ROOTFS_DIR/dev"
	sudo mount -t proc proc "$ROOTFS_DIR/proc"
	sudo mount -t sysfs sysfs "$ROOTFS_DIR/sys"
	sudo chroot "$ROOTFS_DIR" env DEBIAN_FRONTEND=noninteractive bash -c '
		set -euo pipefail
		apt-get update -qq
		grep -q "^deb-src" /etc/apt/sources.list || sed -n "s/^deb /deb-src /p" /etc/apt/sources.list >> /etc/apt/sources.list
		apt-get update -qq
		apt-get install -y dpkg-dev >/dev/null
		rm -rf /tmp/ppp-src && mkdir -p /tmp/ppp-src && cd /tmp/ppp-src && apt-get source ppp
	'
	mkdir -p "$PPPD_HEADER_CACHE"
	sudo cp -r "$ROOTFS_DIR"/tmp/ppp-src/ppp-*/pppd "$PPPD_HEADER_CACHE/pppd"
	sudo rm -rf "$ROOTFS_DIR/tmp/ppp-src"
	sudo umount "$ROOTFS_DIR/dev" "$ROOTFS_DIR/proc" "$ROOTFS_DIR/sys"
	sudo chown -R "$(id -u):$(id -g)" "$PPPD_HEADER_CACHE"
fi

echo "==> cross-building samsungipcd ($(cd "$DAEMON_SRC_DIR" && git rev-parse --short HEAD))"
docker exec "$CONTAINER_NAME" rm -rf "$BUILD_DIR_DAEMON"
docker cp "$DAEMON_SRC_DIR" "$CONTAINER_NAME:$BUILD_DIR_DAEMON"
docker cp "$PPPD_HEADER_CACHE/pppd" "$CONTAINER_NAME:$BUILD_DIR_DAEMON/pppd"
docker exec -w "$BUILD_DIR_DAEMON" "$CONTAINER_NAME" bash -c "rm -rf .git"
docker exec -w "$BUILD_DIR_DAEMON" "$CONTAINER_NAME" bash -c "
	set -euo pipefail
	CC=aarch64-linux-gnu-gcc-6
	\$CC ppp_crc.c -O3 -c -o ppp_crc.o
	\$CC -DPREFIX='\"/usr\"' server.c ppp_crc.o samsung-ipc.c \
		-I$SYSROOT/usr/include/samsung-ipc -L$SYSROOT/usr/lib \
		-lsamsung-ipc -lpthread \
		-Wl,-rpath-link,$SYSROOT/usr/lib \
		-Wl,-rpath-link,$OPENSSL_SYSROOT/usr/lib/aarch64-linux-gnu \
		-o samsungipcd
	\$CC -DPREFIX='\"/usr\"' -DPPPD_VERSION='\"2.4.9\"' -I. \
		ppp_hook.c ppp_crc.o -fPIE -fPIC -shared -pthread -o ppp-hook.so
"

mkdir -p "$SCRIPT_DIR/misc-files/samsungipcd"
docker cp "$CONTAINER_NAME:$BUILD_DIR_DAEMON/samsungipcd" "$SCRIPT_DIR/misc-files/samsungipcd/samsungipcd"
docker cp "$CONTAINER_NAME:$BUILD_DIR_DAEMON/ppp-hook.so" "$SCRIPT_DIR/misc-files/samsungipcd/ppp-hook.so"
docker cp "$CONTAINER_NAME:$SYSROOT/usr/lib/libsamsung-ipc.so.0.0.0" "$SCRIPT_DIR/misc-files/samsungipcd/libsamsung-ipc.so.0.0.0"

echo "==> done: $SCRIPT_DIR/misc-files/samsungipcd/{samsungipcd,ppp-hook.so,libsamsung-ipc.so.0.0.0}"
echo "    run ./setup_mobile_data.sh next to install these into debootstrap-rootfs"
