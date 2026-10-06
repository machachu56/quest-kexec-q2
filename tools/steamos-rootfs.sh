#!/usr/bin/env bash
# Build the SteamOS (Steam Frame "deckard") userspace image for the headset.
#
# Usage: tools/steamos-rootfs.sh [out-dir] [size-GiB]
#
# Valve's published Frame base rootfs (Holo Core, glibc) plus the Frame's
# Steam client, FEX, Xwayland and PipeWire from Valve's repositories
# (tools/steamos/pacman.conf). The 4.19 vendor kernel cannot run this
# systemd, so the image is used as a chroot from the Alpine rootfs
# (rootfs/usr/local/bin/qkx-steamos). Output: <out-dir>/steamos.img (ext4).
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(realpath -m "${1:-$HERE/out/steamos}")
SIZE=${2:-24}
BASE_URL=https://steamdeck-packages.steamos.cloud/archlinux-deckard/archlinux/main/system.rootfs.zst
PKGS="deckard-steam-rel fex-wtf xorg-xwayland vulkan-icd-loader libglvnd mesa \
	pipewire pipewire-pulse wireplumber python sudo which jq xorg-xhost strace"
mkdir -p "$OUT"

if ! podman image exists qkx-deckard-base; then
	[ -f "$OUT/system.rootfs.zst" ] || curl -fL -o "$OUT/system.rootfs.zst" "$BASE_URL"
	zstd -dc "$OUT/system.rootfs.zst" | podman import --arch arm64 - qkx-deckard-base
fi

podman rm -f qkx-steamos-build >/dev/null 2>&1 || true
podman run --platform linux/arm64 --name qkx-steamos-build \
	-v "$HERE/tools/steamos/pacman.conf:/etc/pacman.conf:Z,ro" qkx-deckard-base sh -euc "
pacman -Syu --noconfirm --needed $PKGS >/dev/null
cp /etc/pacman.conf /etc/pacman.conf.qkx 2>/dev/null || true
# No systemd-resolved/machined in the chroot: resolve hosts directly.
sed -i "s/^hosts:.*/hosts: files dns/" /etc/nsswitch.conf
id steam >/dev/null 2>&1 || useradd -m -u 1000 -G video,audio,input -s /bin/bash steam
echo 'steam ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/steam
# Android kernels (CONFIG_ANDROID_PARANOID_NETWORK) only let these groups open
# network sockets.
getent group aid_inet >/dev/null || groupadd -g 3003 aid_inet
getent group aid_net_raw >/dev/null || groupadd -g 3004 aid_net_raw
usermod -aG aid_inet,aid_net_raw steam
# Unpack the client where select_steam.sh/RUNSTEAM.sh expect it.
su steam -c 'mkdir -p ~/.local/share/Steam && tar --zstd -xf /usr/lib/steam/steam.tar.zst -C ~/.local/share/Steam'
cp /usr/share/deckard/RUNSTEAM.sh /home/steam/.local/share/Steam/
chown steam: /home/steam/.local/share/Steam/RUNSTEAM.sh
pacman -Scc --noconfirm >/dev/null
echo packages: \$(pacman -Q | wc -l)
"
# Valve's real pacman.conf points at the same repos; keep ours for updates.
podman cp "$HERE/tools/steamos/pacman.conf" qkx-steamos-build:/etc/pacman.conf

# ext4 with root ownership preserved: build it inside a container.
rm -f "$OUT/steamos.img"
podman export qkx-steamos-build | podman run --rm -i --platform linux/arm64 \
	-v "$OUT:/out:Z" -e SIZE="$SIZE" docker.io/library/alpine:3.22 sh -euc '
apk add -q e2fsprogs tar
mkdir /r && tar -C /r --numeric-owner -xpf -
truncate -s ${SIZE}G /out/steamos.img
mke2fs -q -t ext4 -b 4096 -I 256 -m 0 -L steamos -E nodiscard \
	-O ^orphan_file,^metadata_csum_seed,^casefold,^fast_commit,^large_dir \
	-d /r /out/steamos.img
'
ls -lh "$OUT/steamos.img"
