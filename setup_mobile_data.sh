#!/bin/bash
#
# Installs samsungipcd (built by build_samsungipcd.sh) into
# debootstrap-rootfs along with what lets the standard desktop stack use
# it: ModemManager sees the modem, NetworkManager dials it.
#
# How the pieces fit (see memory/todo_mobile_data_modemmanager.md):
#
#  - The kernel (CONFIG_TTY_GSM_VIRTUAL, drivers/tty/ttygsm.c) registers
#    /dev/ttyGSM0 and /dev/ttyGSM1 as real tty devices plus master nodes
#    /dev/ttygsm-master0/1. samsungipcd opens the master nodes and
#    emulates a modem's AT interface on them on top of Samsung's IPC
#    protocol (boots the J4's modem from the RADIO partition via the
#    herolte/Shannon profile of libsamsung-ipc).
#  - A udev rule groups the two ttys and the modem driver's network
#    interface rmnet0 into one device for ModemManager and ties it to the
#    samsungipcd plugin (rmnet1-7 and umts_dm0 of the same driver are ignored).
#  - The samsungipcd ModemManager plugin (mm-plugin-samsungipcd/, built by
#    build_mm_plugin.sh) is the way regular modems with a network interface are
#    handled: it activates the data session with AT+CGACT, reads the address and
#    DNS with AT+CGCONTRDP and reports them as a static IP configuration on
#    rmnet0. NetworkManager puts that on rmnet0 itself. No PPP, no ppp0 and
#    nothing passes through samsungipcd: the kernel's modem driver carries the
#    packets.
#  - The NetworkManager profile below uses route-metric 900, so WiFi/USB are
#    preferred and mobile data is the fallback.
#
# The first activation after boot can fail while the modem re-registers after
# the LTE attach profile was sent; the plugin retries, and so does NetworkManager.
#
# APN: Telkomsel by default; APN=xyz ./setup_mobile_data.sh for another
# one, or later `nmcli connection modify "Mobile data" gsm.apn xyz`.
#
set -euo pipefail

APN="${APN:-internet}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOTFS_DIR="$SCRIPT_DIR/debootstrap-rootfs"
BUILD_DIR="$SCRIPT_DIR/misc-files/samsungipcd"
PLUGIN_SO="$BUILD_DIR/libmm-plugin-samsungipcd.so"
PLUGIN_DIR="$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/ModemManager"

if [ ! -d "$ROOTFS_DIR" ]; then
	echo "error: missing $ROOTFS_DIR" >&2
	exit 1
fi
if [ ! -x "$BUILD_DIR/samsungipcd" ]; then
	echo "error: $BUILD_DIR/samsungipcd not found - run ./build_samsungipcd.sh first" >&2
	exit 1
fi
if [ ! -f "$PLUGIN_SO" ]; then
	echo "error: $PLUGIN_SO not found - run ./build_mm_plugin.sh first" >&2
	exit 1
fi

echo "==> installing ModemManager (rootfs)"
sudo mount --bind /dev "$ROOTFS_DIR/dev"
sudo mount -t proc proc "$ROOTFS_DIR/proc"
sudo mount -t sysfs sysfs "$ROOTFS_DIR/sys"
sudo chroot "$ROOTFS_DIR" env DEBIAN_FRONTEND=noninteractive apt-get install -y modemmanager
sudo umount "$ROOTFS_DIR/dev" "$ROOTFS_DIR/proc" "$ROOTFS_DIR/sys"

echo "==> installing samsungipcd, libsamsung-ipc.so"
sudo install -D -o root -g root -m 0755 "$BUILD_DIR/samsungipcd" "$ROOTFS_DIR/usr/sbin/samsungipcd"
sudo install -D -o root -g root -m 0755 "$BUILD_DIR/libsamsung-ipc.so.0.0.0" "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/libsamsung-ipc.so.0.0.0"
sudo ln -sf libsamsung-ipc.so.0.0.0 "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/libsamsung-ipc.so.0"
sudo ln -sf libsamsung-ipc.so.0.0.0 "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/libsamsung-ipc.so"
sudo chroot "$ROOTFS_DIR" ldconfig

echo "==> installing the samsungipcd ModemManager plugin"
sudo install -D -o root -g root -m 0755 "$PLUGIN_SO" "$PLUGIN_DIR/libmm-plugin-samsungipcd.so"

echo "==> installing samsungipcd.service (enabled)"
sudo install -d -m 755 "$ROOTFS_DIR/etc/systemd/system"
sudo tee "$ROOTFS_DIR/etc/systemd/system/samsungipcd.service" >/dev/null <<'EOF'
[Unit]
Description=Samsung IPC modem AT command proxy (ss310ap/herolte profile)
# the modem boot reads the NV data from /efs; without this the first start races the mount and fails
RequiresMountsFor=/efs
# ModemManager probes the ports once, when it sees them; samsungipcd has to be
# answering by then or it gives up on them until the next rescan
Before=ModemManager.service

[Service]
# SAMSUNGIPCD_APN: the APN to attach to LTE with as soon as the modem is ready (written below)
EnvironmentFile=-/etc/default/samsungipcd
# stdbuf: stdout is a pipe under systemd, so it would be block buffered
# SyslogLevel: journald.conf here only stores notice and above, the
# daemon's printf output would be dropped otherwise
SyslogLevel=notice
ExecStart=/usr/bin/stdbuf -oL -eL /usr/sbin/samsungipcd /dev/ttygsm-master0 /dev/ttygsm-master1
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
sudo install -d -m 755 "$ROOTFS_DIR/etc/default"
echo "SAMSUNGIPCD_APN=$APN" | sudo tee "$ROOTFS_DIR/etc/default/samsungipcd" >/dev/null
sudo install -d -m 755 "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants"
sudo ln -sf /etc/systemd/system/samsungipcd.service "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/samsungipcd.service"

echo "==> installing the udev rules for the plugin"
sudo install -D -o root -g root -m 0644 "$SCRIPT_DIR/mm-plugin-samsungipcd/77-mm-samsungipcd.rules" \
	"$ROOTFS_DIR/etc/udev/rules.d/77-mm-samsungipcd.rules"

echo "==> installing the NetworkManager mobile broadband profile (APN: $APN)"
sudo install -d -m 755 "$ROOTFS_DIR/etc/NetworkManager/system-connections"
NM_PROFILE="$ROOTFS_DIR/etc/NetworkManager/system-connections/mobile-data.nmconnection"
if [ ! -e "$NM_PROFILE" ]; then
	sudo tee "$NM_PROFILE" >/dev/null <<EOF
[connection]
id=Mobile data
uuid=$(cat /proc/sys/kernel/random/uuid)
type=gsm
autoconnect=true

[gsm]
apn=$APN

[ipv4]
method=auto
route-metric=900

[ipv6]
method=ignore
EOF
	sudo chown root:root "$NM_PROFILE"
	sudo chmod 600 "$NM_PROFILE"
else
	echo "    (profile exists, kept - change it with nmcli)"
fi

echo "==> removing leftovers of the previous setups, if present"
# rmnet0 used to be marked unmanaged; with the plugin NetworkManager configures
# it as the modem's interface and that rule is not needed (nor wanted) any more
sudo rm -f "$ROOTFS_DIR/etc/NetworkManager/conf.d/10-unmanaged-rmnet0.conf" \
	"$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/mobile-data.service" \
	"$ROOTFS_DIR/etc/systemd/system/mobile-data.service" \
	"$ROOTFS_DIR/usr/sbin/mobile-data" \
	"$ROOTFS_DIR/etc/NetworkManager/dispatcher.d/90-mobile-data" \
	"$ROOTFS_DIR/etc/ppp/peers/gprs" \
	"$ROOTFS_DIR/usr/share/samsungipcd/ppp-hook.so"

echo "==> done"
echo
echo "Needs a kernel with CONFIG_TTY_GSM_VIRTUAL (see kernel/); CONFIG_PPP_ASYNC is no longer needed."
echo "Mobile data comes up on its own at boot (NetworkManager retries the first dial)."
echo "Check with: mmcli -L; mmcli -b 0; nmcli connection show --active; ip addr show rmnet0"
