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
#    emulates a modem's AT/PPP interface on them on top of Samsung's IPC
#    protocol (boots the J4's modem from the RADIO partition via the
#    herolte/Shannon profile of libsamsung-ipc).
#  - A udev rule groups the two ttys into one device for ModemManager
#    (ttyGSM0 = primary AT port, ttyGSM1 = PPP data port).
#  - ModemManager dials (ATD) on the data port and reports a PPP bearer;
#    NetworkManager then runs pppd on that port through its pppd plugin.
#    That needs CONFIG_PPP_ASYNC=y in the kernel (N_PPP line discipline).
#    samsungipcd answers LCP/PAP/IPCP itself, NetworkManager gets ppp0
#    with address, routes and DNS from it.
#  - The profile below uses route-metric 900, so WiFi/USB are preferred
#    and mobile data is the fallback.
#
# Note how data flows: samsungipcd injects the IPv4 packets pppd sends
# into rmnet0 itself, replies arrive on rmnet0 and go straight to the
# host stack, so ppp0's RX counters stay near zero and sockets must not
# be bound to ppp0 (ping -I ppp0 cannot work).
#
# The first dial after boot can fail while the modem re-registers after
# the LTE attach profile was sent; NetworkManager retries on its own.
#
# APN: Telkomsel by default; APN=xyz ./setup_mobile_data.sh for another
# one, or later `nmcli connection modify "Mobile data" gsm.apn xyz`.
#
set -euo pipefail

APN="${APN:-internet}"

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

echo "==> installing samsungipcd, libsamsung-ipc.so"
sudo install -D -o root -g root -m 0755 "$BUILD_DIR/samsungipcd" "$ROOTFS_DIR/usr/sbin/samsungipcd"
sudo install -D -o root -g root -m 0755 "$BUILD_DIR/libsamsung-ipc.so.0.0.0" "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/libsamsung-ipc.so.0.0.0"
sudo ln -sf libsamsung-ipc.so.0.0.0 "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/libsamsung-ipc.so.0"
sudo ln -sf libsamsung-ipc.so.0.0.0 "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/libsamsung-ipc.so"
sudo chroot "$ROOTFS_DIR" ldconfig

echo "==> installing samsungipcd.service (enabled)"
sudo install -d -m 755 "$ROOTFS_DIR/etc/systemd/system"
sudo tee "$ROOTFS_DIR/etc/systemd/system/samsungipcd.service" >/dev/null <<'EOF'
[Unit]
Description=Samsung IPC modem AT command proxy (ss310ap/herolte profile)
# ModemManager probes the ports once, when it starts; samsungipcd has to be
# answering by then or it gives up on them until the next rescan
Before=ModemManager.service

[Service]
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
sudo install -d -m 755 "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants"
sudo ln -sf /etc/systemd/system/samsungipcd.service "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/samsungipcd.service"

echo "==> installing the udev rule that groups the ports for ModemManager"
sudo install -d -m 755 "$ROOTFS_DIR/etc/udev/rules.d"
sudo tee "$ROOTFS_DIR/etc/udev/rules.d/77-mm-samsungipcd.rules" >/dev/null <<'EOF'
# samsungipcd modem ports (kernel ttygsm driver): one modem, two AT ports.
# Without a common ID_MM_PHYSDEV_UID ModemManager makes one modem per tty.
ACTION!="add|change|move|bind", GOTO="mm_samsungipcd_end"
SUBSYSTEM!="tty", GOTO="mm_samsungipcd_end"
KERNEL=="ttyGSM[0-9]*", ENV{ID_MM_PHYSDEV_UID}="samsungipcd"
KERNEL=="ttyGSM0", ENV{ID_MM_PORT_TYPE_AT_PRIMARY}="1"
KERNEL=="ttyGSM1", ENV{ID_MM_PORT_TYPE_AT_PPP}="1"
LABEL="mm_samsungipcd_end"
EOF

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

echo "==> NetworkManager: leave the modem's raw rmnet0 alone"
# samsungipcd injects packets into rmnet0 itself (a raw point-to-point
# interface without ARP); NetworkManager managing it would only fight.
sudo install -d -m 755 "$ROOTFS_DIR/etc/NetworkManager/conf.d"
sudo tee "$ROOTFS_DIR/etc/NetworkManager/conf.d/10-unmanaged-rmnet0.conf" >/dev/null <<'EOF'
[keyfile]
unmanaged-devices=interface-name:rmnet0
EOF

echo "==> removing the previous mobile-data service, if present"
sudo rm -f "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/mobile-data.service" \
	"$ROOTFS_DIR/etc/systemd/system/mobile-data.service" \
	"$ROOTFS_DIR/usr/sbin/mobile-data" \
	"$ROOTFS_DIR/etc/NetworkManager/dispatcher.d/90-mobile-data" \
	"$ROOTFS_DIR/etc/ppp/peers/gprs" \
	"$ROOTFS_DIR/usr/share/samsungipcd/ppp-hook.so"

echo "==> done"
echo
echo "Needs a kernel with CONFIG_TTY_GSM_VIRTUAL and CONFIG_PPP_ASYNC (see kernel/)."
echo "Mobile data comes up on its own at boot (NetworkManager retries the first dial)."
echo "Check with: mmcli -L; nmcli connection show --active; ip addr show ppp0"
