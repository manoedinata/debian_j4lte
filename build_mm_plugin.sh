#!/bin/bash
#
# Builds the samsungipcd ModemManager plugin (mm-plugin-samsungipcd/), which
# lets ModemManager use the modem through its network interface (rmnet0) with
# a static IP address read over AT (AT+CGACT / AT+CGCONTRDP), instead of PPP.
#
# ModemManager plugins can only be built inside ModemManager's own source tree,
# so this builds the whole Debian source package of the version installed in
# debootstrap-rootfs, in a throwaway arm64 copy of that rootfs (~/mmbuild-rootfs,
# run under qemu-user), so that none of the build dependencies end up in the
# image that gets flashed. The first run is slow (autoconf and a full compile
# under emulation); later runs only rebuild the plugin's three C files.
#
# Result: misc-files/samsungipcd/libmm-plugin-samsungipcd.so, installed by
# setup_mobile_data.sh into /usr/lib/aarch64-linux-gnu/ModemManager/.
# ModemManager refuses plugins built against another version, so rebuild this
# whenever the modemmanager package in the rootfs changes.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOTFS_DIR="$SCRIPT_DIR/debootstrap-rootfs"
BUILD_ROOT="${MMBUILD_ROOTFS:-$HOME/mmbuild-rootfs}"
PLUGIN_SRC="$SCRIPT_DIR/mm-plugin-samsungipcd"
OUT_DIR="$SCRIPT_DIR/misc-files/samsungipcd"

[ -d "$ROOTFS_DIR" ] || { echo "error: missing $ROOTFS_DIR" >&2; exit 1; }
[ -d "$PLUGIN_SRC/samsungipcd" ] || { echo "error: missing $PLUGIN_SRC/samsungipcd" >&2; exit 1; }
[ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] || { echo "error: qemu-user binfmt for aarch64 is not set up" >&2; exit 1; }

MM_VERSION="$(sudo chroot "$ROOTFS_DIR" dpkg-query -W -f='${Version}' modemmanager)"
MM_UPSTREAM="${MM_VERSION%%-*}"
echo "==> modemmanager $MM_VERSION in the rootfs"

if [ ! -d "$BUILD_ROOT" ]; then
	echo "==> copying the rootfs to $BUILD_ROOT (throwaway build environment)"
	sudo cp -a "$ROOTFS_DIR" "$BUILD_ROOT"
fi
mountpoint -q "$BUILD_ROOT/proc" || sudo mount -t proc proc "$BUILD_ROOT/proc"
mountpoint -q "$BUILD_ROOT/sys"  || sudo mount -t sysfs sysfs "$BUILD_ROOT/sys"
mountpoint -q "$BUILD_ROOT/dev"  || sudo mount --bind /dev "$BUILD_ROOT/dev"
trap 'sudo umount "$BUILD_ROOT/dev" "$BUILD_ROOT/proc" "$BUILD_ROOT/sys" 2>/dev/null || true' EXIT
sudo cp /etc/resolv.conf "$BUILD_ROOT/etc/resolv.conf"

SRC_DIR="$BUILD_ROOT/build/modemmanager-$MM_UPSTREAM"
if [ ! -d "$SRC_DIR" ]; then
	echo "==> installing build dependencies and fetching the source"
	sudo chroot "$BUILD_ROOT" env DEBIAN_FRONTEND=noninteractive bash -c '
		set -e
		apt-get update -qq
		# build-dep wants Vala, whose package is gone from the archive; only the
		# libraries are needed, so install what is available and go on
		apt-get build-dep -y --fix-missing modemmanager || true
		dpkg --remove --force-depends valac valac-bin valac-0.48-vapi libvalacodegen-0.48-0 2>/dev/null || true
		apt-get install -y --no-install-recommends dpkg-dev autoconf automake libtool \
			gtk-doc-tools gettext autopoint pkg-config xsltproc
		mkdir -p /build && cd /build && apt-get source modemmanager
	'
	sudo chown -R "$(id -u):$(id -g)" "$BUILD_ROOT/build"
fi

echo "==> installing the plugin sources into the ModemManager tree"
# copied over, not replaced: the build tree keeps its dependency files in there
mkdir -p "$SRC_DIR/plugins/samsungipcd"
cp -f "$PLUGIN_SRC"/samsungipcd/*.c "$PLUGIN_SRC"/samsungipcd/*.h "$SRC_DIR/plugins/samsungipcd/"
python3 "$PLUGIN_SRC/register_plugin.py" "$SRC_DIR"

if [ ! -f "$SRC_DIR/config.status" ]; then
	echo "==> autoreconf + configure + full build (slow the first time: emulated arm64)"
	sudo chroot "$BUILD_ROOT" env bash -c "
		set -e
		cd /build/modemmanager-$MM_UPSTREAM
		autoreconf -fi
		./configure --prefix=/usr --libdir=/usr/lib/aarch64-linux-gnu --sysconfdir=/etc --localstatedir=/var \
			--with-polkit=strict --with-suspend-resume=systemd --with-systemdsystemunitdir=/lib/systemd/system \
			--disable-gtk-doc --disable-introspection --disable-vala --disable-maintainer-mode
		make -j\$(nproc)
	"
else
	echo "==> rebuilding the plugin (autotools re-run if the registration changed)"
	sudo chroot "$BUILD_ROOT" env bash -c "
		set -e
		cd /build/modemmanager-$MM_UPSTREAM
		make -C plugins libmm-plugin-samsungipcd.la
	"
fi

mkdir -p "$OUT_DIR"
cp "$SRC_DIR/plugins/.libs/libmm-plugin-samsungipcd.so" "$OUT_DIR/libmm-plugin-samsungipcd.so"
echo "==> done: $OUT_DIR/libmm-plugin-samsungipcd.so"
echo "    run ./setup_mobile_data.sh to install it"
