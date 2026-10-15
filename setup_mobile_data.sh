#!/bin/bash
#
# Installs samsungipcd (built by build_samsungipcd.sh) into
# debootstrap-rootfs along with its runtime deps (ModemManager, the
# udev rule it needs, and a systemd unit).
#
# Confirmed working on real hardware (see memory/mobile_data_samsungipcd.md):
# samsungipcd boots the J4's modem from the RADIO partition via the
# herolte (Shannon) profile of libsamsung-ipc and, with the Shannon
# data messages in our libsamsung-ipc fork, gets an LTE/3G packet
# data session from the carrier.
#
# ModemManager cannot auto-detect these ports (ttyGSM0/1 are CUSE
# devices backed by a pty bind-mounted in by cuse_open(), see
# memory/todo_mobile_data_modemmanager.md), so there is no
# NetworkManager "Mobile Broadband" toggle. Instead mobile-data.service
# (/usr/sbin/mobile-data) dials over ttyGSM0 with chat and then runs a
# plain pppd on it: samsungipcd answers LCP/PAP/IPCP itself, pppd creates
# a real ppp0 through the kernel's N_PPP line discipline
# (CONFIG_PPP_ASYNC=y, needs the kernel built with it) and the script adds
# the routes and DNS. This replaced ppp-hook.so, the userspace PPP framing
# plugin needed while the kernel had no PPP_ASYNC.
#
# Note how data flows: samsungipcd injects the IPv4 packets pppd sends
# into rmnet0 itself, replies arrive on rmnet0 and go straight to the
# host stack, so ppp0's RX counters stay near zero and sockets must not
# be bound to ppp0 (ping -I ppp0 cannot work).
#
# Settings live in /etc/mobile-data.conf (APN, route metric, ...).
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

echo "==> installing samsungipcd, libsamsung-ipc.so"
sudo install -D -o root -g root -m 0755 "$BUILD_DIR/samsungipcd" "$ROOTFS_DIR/usr/sbin/samsungipcd"
sudo rm -f "$ROOTFS_DIR/usr/share/samsungipcd/ppp-hook.so"
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
# stdbuf: stdout is a pipe under systemd, so it would be block buffered
# SyslogLevel: journald.conf here only stores notice and above, the
# daemon's printf output would be dropped otherwise
SyslogLevel=notice
ExecStart=/usr/bin/stdbuf -oL -eL /usr/sbin/samsungipcd cuse:/dev/ttyGSM0 cuse:/dev/ttyGSM1
# the pty bind mounts outlive the daemon and block the next start
ExecStopPost=-/bin/umount /dev/ttyGSM0 /dev/ttyGSM1
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
sudo install -d -m 755 "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants"
sudo ln -sf /etc/systemd/system/samsungipcd.service "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/samsungipcd.service"

echo "==> installing /etc/mobile-data.conf (kept if it already exists)"
if [ ! -e "$ROOTFS_DIR/etc/mobile-data.conf" ]; then
	sudo tee "$ROOTFS_DIR/etc/mobile-data.conf" >/dev/null <<'EOF'
# Settings for mobile-data.service
# Carrier APN (Telkomsel: internet)
APN=internet
# Metric of the default route over rmnet0. Higher than WiFi/USB so those
# are preferred while they are up; mobile data is the fallback.
ROUTE_METRIC=900
MTU=1400
EOF
fi

echo "==> installing the pppd peer"
sudo install -d -m 755 "$ROOTFS_DIR/etc/ppp/peers"
sudo tee "$ROOTFS_DIR/etc/ppp/peers/gprs" >/dev/null <<'EOF'
# Used by /usr/sbin/mobile-data after it has dialed with chat. Plain pppd
# on the pty samsungipcd provides; needs a kernel with CONFIG_PPP_ASYNC.
# No "connect" line: the dial (and waiting for the modem line to answer)
# is done by mobile-data. No default route: mobile-data adds routes, the
# peer address equals ours so pppd has nothing to route anyway.
/dev/ttyGSM0
115200
noauth
user ""
password ""
nodetach
noipdefault
ipcp-accept-local
ipcp-accept-remote
usepeerdns
nodefaultroute
lcp-echo-interval 0
# usepeerdns still writes /etc/ppp/resolv.conf, but the Debian ip-up
# script would copy it over /etc/resolv.conf (owned by NetworkManager or
# tailscale here), mobile-data handles DNS itself
ip-up-script /bin/true
ip-down-script /bin/true
EOF
sudo rm -f "$ROOTFS_DIR/etc/chatscripts/gprs" "$ROOTFS_DIR/etc/systemd/system/ppp-gprs.service"

echo "==> installing /usr/sbin/mobile-data and mobile-data.service (enabled)"
sudo tee "$ROOTFS_DIR/usr/sbin/mobile-data" >/dev/null <<'EOF'
#!/bin/bash
# mobile-data run|cleanup|dns - see setup_mobile_data.sh in the build tree
#
#   run      dial, run pppd (ppp0), add routes/DNS, watch the link (the unit)
#   cleanup  remove routes and DNS entries again, stop pppd
#   dns      (re)add the mobile DNS servers to /etc/resolv.conf; NetworkManager
#            rewrites that file on every connection change, its dispatcher
#            hook calls this afterwards
APN=internet
ROUTE_METRIC=900
MTU=1400
AT_DEV=/dev/ttyGSM0
IFACE=ppp0
CHECK_HOST=1.1.1.1
[ -r /etc/mobile-data.conf ] && . /etc/mobile-data.conf
STATE=/run/mobile-data
mkdir -p "$STATE"
PPPD_PID=

log() { echo "mobile-data: $*"; }

dns_add() {
	[ -s "$STATE/dns" ] || return 0
	while read -r ns; do
		grep -qx "nameserver $ns" /etc/resolv.conf || echo "nameserver $ns" >> /etc/resolv.conf
	done < "$STATE/dns"
}

dns_remove() {
	[ -s "$STATE/dns" ] || return 0
	while read -r ns; do
		sed -i "/^nameserver $ns\$/d" /etc/resolv.conf
	done < "$STATE/dns"
}

cleanup() {
	dns_remove
	rm -f "$STATE/dns"
	[ -n "$PPPD_PID" ] && kill "$PPPD_PID" 2>/dev/null
	pkill -x pppd 2>/dev/null
	ip route del "$CHECK_HOST/32" dev "$IFACE" 2>/dev/null
	return 0
}

# Waits until samsungipcd's AT line answers. Right after boot and after a
# pppd session the first command comes back as ERROR (stale bytes on the
# line), so ask until it says OK.
wait_at() {
	local i
	for i in $(seq 10); do
		timeout 6 chat -t 3 ABORT ERROR '' AT OK '\c' <"$AT_DEV" >"$AT_DEV" 2>/dev/null && return 0
		sleep 1
	done
	return 1
}

# One dial attempt: AT dial, pppd on the line, wait for ppp0's address.
# Leaves pppd running (PPPD_PID) with the DNS servers in $STATE/dns.
dial() {
	wait_at || return 1
	timeout 20 chat -s -t 8 ABORT ERROR ABORT 'NO CARRIER' \
		'' AT OK "AT+CGDCONT=1,\"IP\",\"$APN\"" OK 'ATD*99***1#' OK '\c' \
		<"$AT_DEV" >"$AT_DEV" 2>/dev/null || return 1
	rm -f "$STATE/ppp.log" /etc/ppp/resolv.conf
	pppd call gprs debug logfile "$STATE/ppp.log" >/dev/null 2>&1 </dev/null &
	PPPD_PID=$!
	local i
	for i in $(seq 30); do
		ip -4 addr show "$IFACE" 2>/dev/null | grep -q 'inet ' && break
		kill -0 "$PPPD_PID" 2>/dev/null || return 1
		sleep 1
	done
	ip -4 addr show "$IFACE" 2>/dev/null | grep -q 'inet ' || { kill "$PPPD_PID" 2>/dev/null; return 1; }
	sleep 1	# pppd writes /etc/ppp/resolv.conf right after the address
	sed -nE 's/^nameserver ([0-9.]+)$/\1/p' /etc/ppp/resolv.conf 2>/dev/null | grep -v '^0\.0\.0\.0$' | sort -u > "$STATE/dns"
	return 0
}

configure() {
	local dnsip
	ip link set "$IFACE" mtu "$MTU"
	ip link set rmnet0 up 2>/dev/null
	for dnsip in $(cat "$STATE/dns"); do
		ip route replace "$dnsip/32" dev "$IFACE"
	done
	ip route replace default dev "$IFACE" metric "$ROUTE_METRIC"
	dns_add
	log "$IFACE up: $(ip -4 -o addr show "$IFACE" | awk '{print $4}'), dns: $(tr '\n' ' ' < "$STATE/dns")"
}

run() {
	trap 'cleanup; exit 0' TERM INT
	local attempt fails=0
	for attempt in $(seq 40); do
		[ -e "$AT_DEV" ] && dial && break
		log "dial attempt $attempt failed (modem may still be registering), retrying"
		cleanup
		sleep 5
	done
	ip -4 addr show "$IFACE" 2>/dev/null | grep -q 'inet ' || { log "giving up"; exit 1; }
	configure
	while true; do
		sleep 30 & wait $!
		kill -0 "$PPPD_PID" 2>/dev/null || { log "pppd exited"; cleanup; exit 1; }
		# host route so the check goes over mobile data even if WiFi is up;
		# unbound ping because replies arrive on rmnet0, not on ppp0
		ip route replace "$CHECK_HOST/32" dev "$IFACE"
		if ping -c 2 -W 5 "$CHECK_HOST" >/dev/null 2>&1; then
			fails=0
		else
			fails=$((fails + 1))
			log "connectivity check failed ($fails/3)"
			[ "$fails" -ge 3 ] && { cleanup; exit 1; }
		fi
	done
}

case "$1" in
	run) run ;;
	cleanup) cleanup ;;
	dns) dns_add ;;
	*) echo "usage: $0 run|cleanup|dns" >&2; exit 2 ;;
esac
EOF
sudo chmod 755 "$ROOTFS_DIR/usr/sbin/mobile-data"
sudo tee "$ROOTFS_DIR/etc/systemd/system/mobile-data.service" >/dev/null <<'EOF'
[Unit]
Description=Mobile data over rmnet0 (dial via samsungipcd, configure the interface)
Requires=samsungipcd.service
After=samsungipcd.service

[Service]
# see samsungipcd.service for why
SyslogLevel=notice
ExecStart=/usr/sbin/mobile-data run
ExecStopPost=/usr/sbin/mobile-data cleanup
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF
sudo ln -sf /etc/systemd/system/mobile-data.service "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/mobile-data.service"

echo "==> NetworkManager: leave rmnet0 alone, keep the mobile DNS in resolv.conf"
# mobile-data configures rmnet0 itself (a raw point-to-point interface
# without ARP), NetworkManager managing it too would only fight over
# routes. NetworkManager rewrites /etc/resolv.conf on every connection
# change, hence the dispatcher hook.
sudo install -d -m 755 "$ROOTFS_DIR/etc/NetworkManager/conf.d" "$ROOTFS_DIR/etc/NetworkManager/dispatcher.d"
sudo tee "$ROOTFS_DIR/etc/NetworkManager/conf.d/10-unmanaged-rmnet0.conf" >/dev/null <<'EOF'
[keyfile]
unmanaged-devices=interface-name:rmnet0;interface-name:ppp0
EOF
sudo tee "$ROOTFS_DIR/etc/NetworkManager/dispatcher.d/90-mobile-data" >/dev/null <<'EOF'
#!/bin/sh
case "$2" in
	up|down|dhcp4-change|dhcp6-change|connectivity-change) /usr/sbin/mobile-data dns ;;
esac
exit 0
EOF
sudo chmod 755 "$ROOTFS_DIR/etc/NetworkManager/dispatcher.d/90-mobile-data"

echo "==> done"
echo
echo "Mobile data comes up on its own at boot (mobile-data.service, enabled)."
echo "By hand: systemctl start|stop mobile-data. Settings: /etc/mobile-data.conf"
echo "Check with: ip addr show rmnet0; journalctl -u mobile-data -u samsungipcd"
