#!/bin/busybox sh
# Init for the kexec target: USB Ethernet (ECM) gadget with a BusyBox telnet
# shell. The headset is 10.42.0.2; udhcpd hands the host 10.42.0.1.
#
# If /etc/qkx/maps is present this also assembles a custom Android under
# /android, but never starts it: it leaves a shell so a broken boot stays
# debuggable. Run qkx-launch-android when ready.
export PATH=/bin:/sbin
export HOME=/root
export TERM=vt100
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts /config /tmp
mount -t devpts devpts /dev/pts
mount -t configfs configfs /config
mount -t tmpfs tmpfs /tmp
exec >/dev/kmsg 2>&1

log() { echo "qkx-init: $*"; }
# /proc/qkx_bootdone comes from the target kernel patches: 'd' disarms the
# boot watchdog, 'k' is a keepalive, 'r' forces a log-preserving warm reset.
bootdone() { [ -w /proc/qkx_bootdone ] && echo "$1" > /proc/qkx_bootdone; }

log "reached $(uname -r)"
# After kexec a core in deep idle (power collapse) can fail to wake, and the
# next cross-CPU IPI then soft-locks the system (seen on Quest 2 CPU7 during
# BPF JIT). Keep only the shallowest idle state (WFI) until that is fixed.
for st in /sys/devices/system/cpu/cpu*/cpuidle/state*; do
	[ "${st##*state}" = 0 ] || echo 1 > "$st/disable" 2>/dev/null
done
log "deep cpuidle states disabled"
# Status LED (Quest 2 has red/green/blue): visible progress without USB.
led() { for c in red green blue; do
	v=0; [ "$c" = "$1" ] && v=255
	echo $v > /sys/class/leds/$c/brightness 2>/dev/null
done; }
led blue

g=/config/usb_gadget/qkx
mkdir -p "$g/strings/0x409" "$g/configs/c.1/strings/0x409" "$g/functions/ecm.usb0"
echo 0x1d6b > "$g/idVendor"
echo 0x0105 > "$g/idProduct"
echo QKX-RAM > "$g/strings/0x409/serialnumber"
echo 'quest-pro-kexec' > "$g/strings/0x409/manufacturer"
echo 'RAM-only USB Ethernet shell' > "$g/strings/0x409/product"
echo 'ECM' > "$g/configs/c.1/strings/0x409/configuration"
echo 250 > "$g/configs/c.1/MaxPower"
# Fixed MACs keep the host interface name stable across boots.
echo 02:51:4b:58:00:02 > "$g/functions/ecm.usb0/dev_addr"
echo 02:51:4b:58:00:01 > "$g/functions/ecm.usb0/host_addr"
ln -s "$g/functions/ecm.usb0" "$g/configs/c.1/ecm.usb0"

udc=
for _ in 1 2 3 4 5 6 7 8 9 10; do
	for c in /sys/class/udc/*; do
		[ -e "$c" ] || continue
		echo "${c##*/}" > "$g/UDC" && udc=$c && break 2
	done
	log "waiting for UDC"
	bootdone k
	sleep 1
done
if [ -z "$udc" ]; then
	log "no UDC; forcing warm reset so the log survives"
	led red
	bootdone r
	while :; do sleep 60; done
fi
log "bound ${udc##*/}"
# Keep the DWC3 glue device out of runtime suspend.
echo on > "$udc/device/../power/control" 2>/dev/null
# The kexec target skips SMB5's initial status, so dwc3-msm may never see
# VBUS and start a peripheral session (seen on Quest 2). Force device mode.
mode=$udc/device/../mode
usbstate() {
	for e in /sys/class/extcon/*; do log "extcon $(cat $e/name) $(cat $e/state | tr '\n' ' ')"; done
	log "usb psy present=$(cat /sys/class/power_supply/usb/present) online=$(cat /sys/class/power_supply/usb/online) typec=$(cat /sys/class/power_supply/usb/typec_mode)"
	for r in /sys/class/regulator/*; do case "$(cat $r/name)" in *usb*|hsphy*) log "reg $(cat $r/name) $(cat $r/state)";; esac; done
	log "dwc3 mode=$(cat "$mode") udc=$(cat "$udc/state")"
}
usbstate
led green
# No host enumeration within 30 s: keep the log with a warm reset.
(for _ in $(seq 30); do
	[ "$(cat "$udc/state" 2>/dev/null)" = configured ] && { log "host configured gadget"; exit; }
	sleep 1
done
log "gadget not configured by host (state $(cat "$udc/state")); warm reset"
# The 64K retained log only holds late boot; replay the USB bring-up
# (including dev_dbg lines) from the full printk buffer before resetting.
dmesg | grep -iE "dwc3|hsphy|eud|extcon|pdphy|usbpd|smb5|ssphy|qmp|udc|gadget" |
	grep -v "post-bind dump" | tail -120 |
	while IFS= read -r l; do echo "qkx-replay: $l" > /dev/kmsg; done
led red; sleep 1; bootdone r) &
usbwatch=$!

ifconfig lo 127.0.0.1 up
ifconfig usb0 10.42.0.2 netmask 255.255.255.0 up
cat > /tmp/udhcpd.conf <<EOC
interface usb0
start 10.42.0.1
end 10.42.0.1
max_leases 1
lease_file /tmp/udhcpd.leases
option subnet 255.255.255.0
option lease 86400
EOC
touch /tmp/udhcpd.leases
udhcpd /tmp/udhcpd.conf
# Respawn telnetd: the new OS shares our process table and has killed it before.
(while :; do telnetd -F -l /bin/sh -p 23; log "telnetd exited ($?); restarting"; sleep 1; done) &
echo $! > /tmp/telnet.pid

bootdone d

# Keep a full kernel log on hand: the shell arrives long after early boot, and
# BusyBox dmesg cannot follow the log by itself.
qkx-klogwatch -f /tmp/kernel.log >/dev/null 2>&1 &

if [ -s /etc/qkx/maps/rootfs.map ]; then
	# Plain Linux: mount the pinned rootfs and hand PID 1 to its init. Any
	# failure leaves this shell running for debugging.
	if qkx-boot-linux; then
		log 'switching to Linux rootfs'
		# The USB watchdog subshell would lose its tools with the initramfs
		# and misread the gadget as unconfigured; the host has it by now.
		kill $usbwatch $(cat /tmp/telnet.pid) 2>/dev/null
		killall telnetd udhcpd qkx-klogwatch 2>/dev/null
		umount /config /dev/pts /tmp 2>/dev/null
		exec switch_root /newroot /sbin/init
	fi
	log 'Linux rootfs failed; staying in the initramfs shell'
elif [ -d /etc/qkx/maps ]; then
	log 'assembling custom OS'
	# Keep the transcript: the shell only arrives after this has run. Avoid a
	# pipeline so the exit status is qkx-mount-os's own.
	qkx-mount-os >/tmp/mount-os.log 2>&1
	status=$?
	cat /tmp/mount-os.log
	if [ $status -eq 0 ] && [ -f /etc/qkx/turnkey.json ]; then
		log 'turnkey OS mounted; launching Android automatically'
		qkx-launch-android
	elif [ $status -eq 0 ]; then
		log 'custom OS mounted at /android; run qkx-launch-android to start it'
	else
		log "qkx-mount-os failed (status $status); see /tmp/mount-os.log"
	fi
fi

log "ready: telnet 10.42.0.2"
# Only PID 1 may switch_root, so qkx-launch-android asks this loop to do it.
while [ ! -e /tmp/qkx-launch ]; do sleep 1; done
exec qkx-switch
