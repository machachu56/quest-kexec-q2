#!/usr/bin/env bash
# Download, verify and build a static ARM64 BusyBox.
# Usage: busybox.sh <output-binary>
set -euo pipefail
VER=1.36.1
SHA=b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314
mkdir -p "$(dirname "$1")"
OUT=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
WORK=$(dirname "$OUT")/busybox-build
CROSS=${CROSS_COMPILE-aarch64-linux-gnu-}
mkdir -p "$WORK"

# Some distributions (e.g. Fedora) ship a kernel-only aarch64 cross compiler
# without a C library: build natively in an arm64 Alpine container instead
# (needs podman and qemu-user-static).
if [ -n "$CROSS" ] && ! echo 'int main(void){return 0;}' |
	"${CROSS}gcc" -static -x c - -o "$WORK/cc-test" 2>/dev/null; then
	echo "busybox: ${CROSS}gcc cannot link static binaries; building in an arm64 container"
	HERE=$(cd "$(dirname "$0")" && pwd)
	podman run --rm --platform linux/arm64 -e CROSS_COMPILE= \
		-v "$HERE:/src:Z,ro" -v "$(dirname "$OUT"):/out:Z" docker.io/library/alpine:3.22 \
		sh -euc 'apk add -q bash build-base curl linux-headers perl; bash /src/busybox.sh /out/'"$(basename "$OUT")"
	exit
fi
cd "$WORK"
[ -f busybox-$VER.tar.bz2 ] ||
	curl -fL -o busybox-$VER.tar.bz2 https://busybox.net/downloads/busybox-$VER.tar.bz2
echo "$SHA  busybox-$VER.tar.bz2" | sha256sum -c -
rm -rf busybox-$VER
tar xjf busybox-$VER.tar.bz2
cd busybox-$VER
M="make ARCH=arm64 CROSS_COMPILE=$CROSS"
$M defconfig >/dev/null
# Static, and drop tc: it doesn't build against current kernel headers.
sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/; s/^CONFIG_TC=y/# CONFIG_TC is not set/' .config
$M oldconfig </dev/null >/dev/null 2>&1 || true
grep -q '^CONFIG_STATIC=y' .config
$M -j"$(nproc)" >/dev/null
cp busybox "$OUT"
echo "$OUT"
