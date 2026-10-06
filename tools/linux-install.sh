#!/usr/bin/env bash
# Install a Linux rootfs image into a pinned file on userdata and build a
# kexec payload that boots it.
#
# Usage: tools/linux-install.sh <image> <captured-dir> <target-Image> [out-dir]
#   QKX_NAME=steamos installs an extra image (default: rootfs, the boot root).
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
NAME=${QKX_NAME:-rootfs}

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

if have "$W/img/$NAME.img" && [ -z "${QKX_REINSTALL:-}" ]; then
	say "$NAME: already installed (QKX_REINSTALL=1 to replace)"
else
	sh_ "rm -f $W/img/$NAME.img $W/maps/$NAME.map"
	bytes=$(stat -c %s "$SRC")
	say "$NAME: allocating $((bytes / 1024 / 1024)) MiB pinned"
	sh_ "sh $W/qkx-install.sh alloc $NAME $bytes"
	say "$NAME: staging compressed image"
	gzip -1 -c "$SRC" > "$OUT/$NAME.img.gz"
	adb push "$OUT/$NAME.img.gz" "$W/stage.img.gz" >/dev/null
	say "$NAME: writing raw blocks"
	sh_ "sh $W/qkx-install.sh writez $NAME $W/stage.img.gz"
	say "$NAME: verifying"
	v=$(sh_ "sh $W/qkx-install.sh verify $NAME $W/stage.img.gz" 2>&1)
	printf '%s\n' "$v"
	grep -q 'qkx_rawcp: verified' <<<"$v" || { echo 'raw verification failed'; exit 1; }
	sh_ "rm -f $W/stage.img.gz"
fi

adb exec-out "su -c 'sh $W/qkx-install.sh map $NAME'" </dev/null | tr -d '\r' > "$OSDIR/maps/$NAME.map"
awk 'NF != 3 { exit 1 }' "$OSDIR/maps/$NAME.map" && [ -s "$OSDIR/maps/$NAME.map" ] ||
	{ echo "invalid $NAME map"; exit 1; }
echo "$NAME map: $(wc -l < "$OSDIR/maps/$NAME.map") extents"

say 'building payload'
python3 "$HERE/initramfs/build.py" --busybox "$HERE/out/busybox" --output "$OUT/initramfs.gz" \
	--os-dir "$OSDIR" --qkx-dm "$HERE/tools/device/qkx_dm"
"$HERE/tools/prep.sh" "$C" "$IMG" "$OUT/initramfs.gz" "$OUT/payload" qkx_keep_wdt=1 softlockup_panic=1 hung_task_panic=1 hung_task_timeout_secs=120 ${QKX_EXTRA_CMDLINE:-}
echo "next: tools/run.sh $OUT/payload watchdog_recovery=1"
