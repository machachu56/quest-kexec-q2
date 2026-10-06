#!/usr/bin/env bash
# Share this PC's internet connection with the headset (10.42.0.0/24 over the
# USB Ethernet link) by NAT. Runtime-only rules: they disappear at reboot.
#   sudo tools/host-share-internet.sh on|off
# Handles firewalld (zone of the default-route interface) and Docker's
# FORWARD DROP policy when present; otherwise plain iptables.
set -euo pipefail
NET=10.42.0.0/24
ACTION=${1:-on}
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)"; exit 1; }
WAN=$(ip route show default | awk '{ print $5; exit }')
[ -n "$WAN" ] || { echo "no default route"; exit 1; }

if [ "$ACTION" = on ]; then
	sysctl -qw net.ipv4.ip_forward=1
	if systemctl is-active -q firewalld 2>/dev/null; then
		ZONE=$(firewall-cmd --get-zone-of-interface="$WAN" 2>/dev/null || firewall-cmd --get-default-zone)
		firewall-cmd -q --zone="$ZONE" --add-masquerade
		firewall-cmd -q --zone="$ZONE" --add-forward 2>/dev/null || true
		echo "firewalld: masquerade on zone $ZONE"
	else
		iptables -t nat -C POSTROUTING -s $NET -o "$WAN" -j MASQUERADE 2>/dev/null ||
			iptables -t nat -A POSTROUTING -s $NET -o "$WAN" -j MASQUERADE
		iptables -C FORWARD -s $NET -j ACCEPT 2>/dev/null || iptables -I FORWARD -s $NET -j ACCEPT
		iptables -C FORWARD -d $NET -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null ||
			iptables -I FORWARD -d $NET -m state --state RELATED,ESTABLISHED -j ACCEPT
		echo "iptables: masquerade via $WAN"
	fi
	# Docker sets FORWARD's policy to DROP; its DOCKER-USER chain runs first.
	if iptables -n -L DOCKER-USER >/dev/null 2>&1; then
		iptables -C DOCKER-USER -s $NET -j ACCEPT 2>/dev/null || iptables -I DOCKER-USER -s $NET -j ACCEPT
		iptables -C DOCKER-USER -d $NET -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null ||
			iptables -I DOCKER-USER -d $NET -m state --state RELATED,ESTABLISHED -j ACCEPT
		echo "docker: forwarding allowed for $NET"
	fi
else
	if systemctl is-active -q firewalld 2>/dev/null; then
		ZONE=$(firewall-cmd --get-zone-of-interface="$WAN" 2>/dev/null || firewall-cmd --get-default-zone)
		firewall-cmd -q --zone="$ZONE" --remove-masquerade || true
		firewall-cmd -q --zone="$ZONE" --remove-forward 2>/dev/null || true
	else
		iptables -t nat -D POSTROUTING -s $NET -o "$WAN" -j MASQUERADE 2>/dev/null || true
		iptables -D FORWARD -s $NET -j ACCEPT 2>/dev/null || true
		iptables -D FORWARD -d $NET -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || true
	fi
	iptables -D DOCKER-USER -s $NET -j ACCEPT 2>/dev/null || true
	iptables -D DOCKER-USER -d $NET -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || true
	echo "sharing off (ip_forward left as is)"
fi
