#!/usr/bin/env bash
# Incrementally build headset userspace components in the arm64 builder.
#
# Usage: tools/build-userspace.sh mesa|gamescope
#
# Source trees and build directories persist under out/build/<name>/, so a
# rebuild after a patch change only recompiles what changed (plus ccache).
# Patches come from patches/<name>/*.patch; when that set changes the tree is
# reset to its pristine git baseline and the patches are re-applied.
# Output: out/build/<name>.tar.gz, to extract on the headset with tar -C / -xzf.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
NAME=${1:?mesa|gamescope}
B=$HERE/out/build
mkdir -p "$B/$NAME" "$B/ccache" "$HERE/patches/$NAME"

case "$NAME" in
mesa)
	VER=${MESA_VERSION:-25.1.9}
	TARBALL=$HERE/out/src/mesa-$VER.tar.xz
	[ -f "$TARBALL" ] || curl -fL -o "$TARBALL" "https://archive.mesa3d.org/mesa-$VER.tar.xz"
	FETCH="tar xJf /src/mesa-$VER.tar.xz && mv mesa-$VER src"
	SETUP="-Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm,kgsl -Dgallium-drivers= \
		-Dplatforms=x11,wayland -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled \
		-Dopengl=false -Dllvm=disabled -Dbuildtype=release -Dprefix=/usr/local"
	PACK="./usr/local/lib/libvulkan_freedreno.so ./usr/local/share/vulkan"
	;;
gamescope)
	VER=${GAMESCOPE_VERSION:-3.16.4}
	FETCH="git clone -q --depth 1 --branch $VER https://github.com/ValveSoftware/gamescope src && \
		git -C src submodule update -q --init --depth 1"
	SETUP="-Ddefault_library=static -Ddrm_backend=enabled -Dsdl2_backend=disabled \
		-Dpipewire=enabled -Drt_cap=enabled -Dinput_emulation=enabled \
		-Davif_screenshots=disabled -Dbuildtype=release -Dprefix=/usr/local \
		-Dcpp_args=-DRTLD_DEEPBIND=0"
	PACK="./usr/local/bin/gamescope ./usr/local/share/gamescope"
	# Pinned like Alpine's recipe: current stb only ships stb_image_resize2.h.
	STB=${GAMESCOPE_STB:-5736b15f7ea0ffb08dd38af21067c314d6a3aae9}
	[ -f "$HERE/out/src/stb_image_resize.h" ] || curl -fL -o "$HERE/out/src/stb_image_resize.h" \
		"https://raw.githubusercontent.com/nothings/stb/$STB/stb_image_resize.h"
	EXTRA="cp /src/stb_image_resize.h src/"
	;;
*) echo "unknown component $NAME"; exit 1 ;;
esac

PSUM=$(cat "$HERE/patches/$NAME"/*.patch 2>/dev/null | sha256sum | cut -c1-16)
podman run --rm --platform linux/arm64 -e PSUM="$PSUM" \
	-e CCACHE_DIR=/ccache -v "$B/ccache:/ccache:Z" \
	-v "$HERE/out/src:/src:Z" -v "$HERE/patches/$NAME:/patches:Z,ro" \
	-v "$B/$NAME:/w:Z" -w /w qkx-builder sh -euc "
if [ ! -d src ]; then
	$FETCH
	cd src
	[ -d .git ] || { git init -q && git add -A && git -c user.name=b -c user.email=b@b commit -qm base; }
	cd ..
fi
cd src
if [ \"\$(cat ../patches.stamp 2>/dev/null)\" != \"\$PSUM\" ]; then
	git reset -q --hard HEAD && git clean -qfd -e build
	git submodule foreach -q 'git reset -q --hard HEAD && git clean -qfd' 2>/dev/null || true
	for p in /patches/*.patch; do [ -e \"\$p\" ] && patch -sp1 < \"\$p\" && echo \"applied \${p##*/}\"; done
	echo \"\$PSUM\" > ../patches.stamp
fi
${EXTRA:-true}
[ -f build/build.ninja ] || CC='ccache gcc' CXX='ccache g++' meson setup build $SETUP
ninja -C build -j\$(nproc)
rm -rf /w/inst && DESTDIR=/w/inst ninja -C build install >/dev/null
tar -C /w/inst -czf /w/out.tar.gz $PACK
"
mv "$B/$NAME/out.tar.gz" "$B/$NAME.tar.gz"
ls -l "$B/$NAME.tar.gz"
