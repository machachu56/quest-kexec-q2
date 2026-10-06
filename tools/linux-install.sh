#!/usr/bin/env bash
# Install a Linux rootfs image into a pinned file on userdata and build a
# kexec payload that boots it.
#
# Usage: tools/linux-install.sh <rootfs.img> <captured-dir> <target-Image> [out-dir]
#
# Same storage scheme as os-install.sh: contents go to the raw userdata blocks
# owned by a pinned f2fs file, which is what the kexec target can read. Only
# blocks allocated to that file are written; nothing is formatted.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:?rootfs.img} C=${2:?captured dir} IMG=${3:?target Image}
OUT=${4:-$HERE/out/linux}
W=/data/local/tmp/qkx-linux
OSDIR=$OUT/os

say() { printf '\n== %s\n' "$*"; }
sh_() { adb shell "su -c '$*'"; }
have() { adb exec-out "su -c 'test -f $1 && echo QKX_YES'" | grep -q QKX_YES; }

adb shell 'su -c id' | grep -q 'uid=0' || { echo 'rooted adb (su) required'; exit 1; }
mkdir -p "$OSDIR/maps"

say 'pushing helpers'
adb push "$HERE/tools/device/qkx_fsmap" "$HERE/tools/device/qkx_rawcp" \
	"$HERE/tools/device/qkx_dm" "$HERE/tools/device/qkx-install.sh" \
	/data/local/tmp/ >/dev/null
sh_ "mkdir -p $W/bin $W/img $W/maps &&
     cp /data/local/tmp/qkx_fsmap /data/local/tmp/qkx_rawcp /data/local/tmp/qkx_dm $W/bin/ &&
     cp /data/local/tmp/qkx-install.sh $W/ && chmod 755 $W/bin/* $W/qkx-install.sh"
adb exec-out "su -c 'sh $W/qkx-install.sh partitions'" | tr -d '\r' > "$OSDIR/partitions"
grep -q '^userdata ' "$OSDIR/partitions" || { echo 'partition map has no userdata'; exit 1; }

if have "$W/img/rootfs.img" && [ -z "${QKX_REINSTALL:-}" ]; then
	say 'rootfs: already installed (QKX_REINSTALL=1 to replace)'
else
	sh_ "rm -f $W/img/rootfs.img $W/maps/rootfs.map"
	bytes=$(stat -c %s "$SRC")
	say "rootfs: allocating $((bytes / 1024 / 1024)) MiB pinned"
	sh_ "sh $W/qkx-install.sh alloc rootfs $bytes"
	say 'rootfs: staging compressed image'
	gzip -1 -c "$SRC" > "$OUT/rootfs.img.gz"
	adb push "$OUT/rootfs.img.gz" "$W/stage.img.gz" >/dev/null
	say 'rootfs: writing raw blocks'
	sh_ "sh $W/qkx-install.sh writez rootfs $W/stage.img.gz"
	say 'rootfs: verifying'
	v=$(sh_ "sh $W/qkx-install.sh verify rootfs $W/stage.img.gz" 2>&1)
	printf '%s\n' "$v"
	grep -q 'qkx_rawcp: verified' <<<"$v" || { echo 'raw verification failed'; exit 1; }
	sh_ "rm -f $W/stage.img.gz"
fi

adb exec-out "su -c 'sh $W/qkx-install.sh map rootfs'" </dev/null | tr -d '\r' > "$OSDIR/maps/rootfs.map"
awk 'NF != 3 { exit 1 }' "$OSDIR/maps/rootfs.map" && [ -s "$OSDIR/maps/rootfs.map" ] ||
	{ echo 'invalid rootfs map'; exit 1; }
echo "rootfs map: $(wc -l < "$OSDIR/maps/rootfs.map") extents"

say 'building payload'
python3 "$HERE/initramfs/build.py" --busybox "$HERE/out/busybox" --output "$OUT/initramfs.gz" \
	--os-dir "$OSDIR" --qkx-dm "$HERE/tools/device/qkx_dm"
"$HERE/tools/prep.sh" "$C" "$IMG" "$OUT/initramfs.gz" "$OUT/payload" qkx_keep_wdt=1 ${QKX_EXTRA_CMDLINE:-}
echo "next: tools/run.sh $OUT/payload watchdog_recovery=1"
