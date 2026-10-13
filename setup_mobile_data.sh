#!/bin/bash
#
# Installs samsungipcd (built by build_samsungipcd.sh) into
# debootstrap-rootfs along with its runtime deps (ModemManager, the
# udev rule it needs, and a systemd unit).
#
# Confirmed working on real hardware (see memory/mobile_data_samsungipcd.md):
# the herolte CP-boot path (wrong chip - Shannon333/S7, not the J4's
# Shannon310 - but close enough a generation) actually boots the J4's
# real modem from the RADIO partition and gets it registered on a live
# cell tower; the AT command interface over ttyGSM0/1 is stable after
# a local-echo feedback-loop bug was fixed upstream in the samsungipcd
# submodule itself. samsungipcd.service is now enabled.
#
# ModemManager cannot auto-detect these ports (ttyGSM0/1 are backed by
# a plain Unix98 pty bind-mounted in by cuse_open(), which has no
# sysfs/udev presence by design - confirmed, not a config problem), so
# this sets up a direct pppd+chat dial-up instead: `pon gprs`/`poff gprs`,
# or the ppp-gprs.service wrapper below. No NetworkManager/GNOME
# "Mobile Broadband" toggle - start/stop it like unudhcpd.service.
#
# APN is hardcoded below for Telkomsel ("internet") - change
# /etc/chatscripts/gprs if the SIM changes.
#
set -euo pipefail

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

echo "==> installing samsungipcd, libsamsung-ipc.so, ppp-hook.so"
sudo install -D -o root -g root -m 0755 "$BUILD_DIR/samsungipcd" "$ROOTFS_DIR/usr/sbin/samsungipcd"
sudo install -D -o root -g root -m 0755 "$BUILD_DIR/ppp-hook.so" "$ROOTFS_DIR/usr/share/samsungipcd/ppp-hook.so"
sudo install -D -o root -g root -m 0755 "$BUILD_DIR/libsamsung-ipc.so.0.0.0" "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/libsamsung-ipc.so.0.0.0"
sudo ln -sf libsamsung-ipc.so.0.0.0 "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/libsamsung-ipc.so.0"
sudo ln -sf libsamsung-ipc.so.0.0.0 "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/libsamsung-ipc.so"
sudo chroot "$ROOTFS_DIR" ldconfig

echo "==> installing samsungipcd.service (enabled)"
sudo install -d -m 755 "$ROOTFS_DIR/etc/systemd/system"
sudo tee "$ROOTFS_DIR/etc/systemd/system/samsungipcd.service" >/dev/null <<'EOF'
[Unit]
Description=Samsung IPC modem AT command proxy (ss310ap/herolte profile)

[Service]
ExecStart=/usr/sbin/samsungipcd cuse:/dev/ttyGSM0 cuse:/dev/ttyGSM1
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
sudo install -d -m 755 "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants"
sudo ln -sf /etc/systemd/system/samsungipcd.service "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/samsungipcd.service"

echo "==> installing the GPRS chat script and pppd peer"
sudo install -d -m 755 "$ROOTFS_DIR/etc/chatscripts"
sudo tee "$ROOTFS_DIR/etc/chatscripts/gprs" >/dev/null <<'EOF'
# samsungipcd replies "OK" to the dial command (ATD*99***1#), not the
# usual "CONNECT" - see server.c's handle_at_command().
ABORT 'ERROR'
ABORT 'NO CARRIER'
ABORT 'NO DIALTONE'
ABORT 'BUSY'
'' AT
OK AT+CGDCONT=1,"IP","internet"
OK ATD*99***1#
OK \c
EOF
sudo install -d -m 755 "$ROOTFS_DIR/etc/ppp/peers"
sudo tee "$ROOTFS_DIR/etc/ppp/peers/gprs" >/dev/null <<'EOF'
# Direct dial-up over samsungipcd's AT proxy (ttyGSM0). See
# setup_mobile_data.sh for why this exists instead of ModemManager.
#
# Do NOT add lock/crtscts/nocrtscts/local here: those options are
# registered by pppd's *built-in* tty channel (pppd/tty.c's
# tty_options[]), which ppp-hook.so's plugin_init() replaces
# (`the_channel = &tty_channel`, pointing at ppp_hook.c's own struct
# with .options left NULL). Once the plugin's loaded (processing the
# `plugin` line above), any later option pppd doesn't otherwise
# recognize falls through to `the_channel->options` in find_option()
# (options.c) and dereferences it unconditionally - a NULL options
# table means an instant SIGSEGV the moment any tty-specific option is
# parsed. Confirmed via strace with `local` present: pppd segfaults
# (SIGSEGV, si_addr=NULL) right after loading the plugin, with no
# further syscalls in between - find_option()'s string compares don't
# touch the kernel, so the silence is expected. None of these options
# apply anyway: there's no real serial line here, so there's no
# carrier-detect or flow control to configure.
/dev/ttyGSM0
115200
connect "/usr/sbin/chat -v -f /etc/chatscripts/gprs"
plugin /usr/share/samsungipcd/ppp-hook.so
noauth
user ""
password ""
nodetach
noipdefault
ipcp-accept-local
ipcp-accept-remote
defaultroute
usepeerdns
lcp-echo-interval 30
lcp-echo-failure 3
EOF

echo "==> installing ppp-gprs.service (not enabled - start by hand: systemctl start ppp-gprs)"
sudo tee "$ROOTFS_DIR/etc/systemd/system/ppp-gprs.service" >/dev/null <<'EOF'
[Unit]
Description=Mobile data (pppd dial-up over samsungipcd's AT proxy)
Requires=samsungipcd.service
After=samsungipcd.service

[Service]
ExecStart=/usr/sbin/pppd call gprs
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

echo "==> telling NetworkManager to leave rmnet0 and ppp0 alone"
# samsungipcd talks to rmnet0 directly via a raw AF_PACKET/AF_INET
# socket (see INTERFACE_NAME in samsung-ipc.c) - it's not a normal IP
# netdev to configure/DHCP. ppp0 is brought up and fully configured by
# pppd itself (see peers/gprs: defaultroute, usepeerdns) - NetworkManager
# managing it too would just fight pppd over routes/DNS.
sudo install -d -m 755 "$ROOTFS_DIR/etc/NetworkManager/conf.d"
sudo tee "$ROOTFS_DIR/etc/NetworkManager/conf.d/10-unmanaged-rmnet0.conf" >/dev/null <<'EOF'
[keyfile]
unmanaged-devices=interface-name:rmnet0;interface-name:ppp0
EOF

echo "==> done"
echo
echo "To bring mobile data up: systemctl start ppp-gprs (samsungipcd starts automatically"
echo "as its dependency). To make it persistent across boots: systemctl enable ppp-gprs."
echo "Check with: ip addr show ppp0 / journalctl -u ppp-gprs -u samsungipcd"
