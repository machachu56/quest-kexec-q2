#!/usr/bin/env bash
# Build a minimal ARM64 Linux root filesystem image for the kexec target.
#
# Usage: tools/linux-rootfs.sh [out-dir] [size-GiB]
#
# Alpine (OpenRC) is used for bring-up: it runs on the 4.19 vendor kernel,
# unlike current systemd distributions. Built in an arm64 podman container
# (needs qemu-user-static). Output: <out-dir>/rootfs.img (ext4) and an SSH key.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(realpath -m "${1:-$HERE/out/linux}")
SIZE=${2:-8}
ALPINE=${ALPINE:-3.22}
mkdir -p "$OUT"

[ -f "$OUT/id_ed25519" ] || ssh-keygen -q -t ed25519 -N '' -C qkx-quest -f "$OUT/id_ed25519"

podman run --rm --platform linux/arm64 -v "$OUT:/out:Z" -e SIZE="$SIZE" \
	-v "$HERE/rootfs:/overlay:Z,ro" \
	"docker.io/library/alpine:$ALPINE" sh -eu -c '
apk add -q e2fsprogs
R=/tmp/rootfs
mkdir -p $R/etc/apk
cp /etc/apk/repositories $R/etc/apk/
apk add -q --root $R --initdb --allow-untrusted --keys-dir /etc/apk/keys \
	alpine-base openrc busybox-extras busybox-extras-openrc openssh \
	e2fsprogs util-linux bash htop iproute2 kmod pciutils usbutils py3-evdev
cp /etc/apk/keys/* $R/etc/apk/keys/

echo quest2 > $R/etc/hostname
cat > $R/etc/network/interfaces <<EOF
auto lo
iface lo inet loopback

# USB Ethernet gadget, bound by the initramfs before switch_root.
auto usb0
iface usb0 inet static
	address 10.42.0.2
	netmask 255.255.255.0
	# The host shares its internet (NAT) over the USB link.
	gateway 10.42.0.1
EOF
printf "nameserver 1.1.1.1\nnameserver 9.9.9.9\n" > $R/etc/resolv.conf
cat > $R/etc/udhcpd.conf <<EOF
interface usb0
start 10.42.0.1
end 10.42.0.1
max_leases 1
lease_file /var/lib/udhcpd.leases
option subnet 255.255.255.0
option lease 86400
EOF
mkdir -p $R/var/lib && touch $R/var/lib/udhcpd.leases
echo "/dev/root / ext4 rw,noatime 0 1" > $R/etc/fstab
# No VT console on the headset: drop the getty lines.
sed -i "/^tty[0-9]/d" $R/etc/inittab
# Same uid as the SteamOS image'"'"'s steam user: Xwayland (here) resolves
# xhost si:localuser:steam against this passwd.
chroot $R adduser -D -u 1000 -s /bin/sh steam
mkdir -p -m 700 $R/root/.ssh
cp /out/id_ed25519.pub $R/root/.ssh/authorized_keys
chmod 600 $R/root/.ssh/authorized_keys
sed -i "s/^#\?PermitRootLogin.*/PermitRootLogin prohibit-password/" $R/etc/ssh/sshd_config

for s in devfs dmesg mdev hwdrivers; do ln -sf /etc/init.d/$s $R/etc/runlevels/sysinit/$s; done
for s in bootmisc hostname modules sysctl syslog; do ln -sf /etc/init.d/$s $R/etc/runlevels/boot/$s; done
# Repo overlay (launchers, input receiver, services).
cp -a /overlay/. $R/
for s in networking sshd udhcpd local qkx-input qkx-controllers; do ln -sf /etc/init.d/$s $R/etc/runlevels/default/$s; done

# Features the 4.19 target understands (newer e2fsprogs defaults are not).
rm -f /out/rootfs.img
truncate -s ${SIZE}G /out/rootfs.img
mke2fs -q -t ext4 -b 4096 -I 256 -m 0 -L qkxroot -E nodiscard \
	-O ^orphan_file,^metadata_csum_seed,^casefold,^fast_commit,^large_dir \
	-d $R /out/rootfs.img
'
ls -lh "$OUT/rootfs.img"
echo "rootfs ready: $OUT/rootfs.img (ssh -i $OUT/id_ed25519 root@10.42.0.2)"
