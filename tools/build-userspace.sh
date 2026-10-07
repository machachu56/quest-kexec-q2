#!/usr/bin/env bash
# Incrementally build headset userspace components in the arm64 builder.
#
# Usage: tools/build-userspace.sh mesa|mesa-holo|gamescope|monado|hello_xr
#   mesa-holo: the same Mesa for the glibc SteamOS chroot (qkx-holo-builder).
#
# Source trees and build directories persist under out/build/<name>/, so a
# rebuild after a patch change only recompiles what changed (plus ccache).
# Patches come from patches/<name>/*.patch; when that set changes the tree is
# reset to its pristine git baseline and the patches are re-applied.
# Output: out/build/<name>.tar.gz, to extract on the headset with tar -C / -xzf.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
NAME=${1:?mesa|mesa-holo|gamescope|monado|hello_xr}
BUILDER=qkx-builder
B=$HERE/out/build
mkdir -p "$B/$NAME" "$B/ccache"

case "$NAME" in
mesa)
	VER=${MESA_VERSION:-25.1.9}
	TARBALL=$HERE/out/src/mesa-$VER.tar.xz
	[ -f "$TARBALL" ] || curl -fL -o "$TARBALL" "https://archive.mesa3d.org/mesa-$VER.tar.xz"
	FETCH="tar xJf /src/mesa-$VER.tar.xz && mv mesa-$VER src"
	# Turnip on KGSL for Vulkan, and Zink on top of it for OpenGL/GLES/EGL/GBM
	# (Xwayland glamor, Steam's UI, GL games): there is no GL driver for KGSL.
	SETUP="-Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm,kgsl -Dgallium-drivers=zink \
		-Dplatforms=x11,wayland -Dglx=dri -Degl=enabled -Dgbm=enabled -Dgles1=disabled \
		-Dgles2=enabled -Dopengl=true -Dllvm=disabled -Dglvnd=disabled \
		-Dbuildtype=release -Dprefix=/usr/local"
	PACK="./usr/local"
	;;
mesa-holo)
	BUILDER=qkx-holo-builder
	VER=${MESA_VERSION:-25.1.9}
	TARBALL=$HERE/out/src/mesa-$VER.tar.xz
	[ -f "$TARBALL" ] || curl -fL -o "$TARBALL" "https://archive.mesa3d.org/mesa-$VER.tar.xz"
	# glibc >= 2.43 declares call_once/once_flag in <stdlib.h> (_GNU_SOURCE
	# implies C23): keep Mesa's C11 threads emulation but take those two from
	# glibc instead of redefining them.
	FETCH="tar xJf /src/mesa-$VER.tar.xz && mv mesa-$VER src && \
		perl -0pi -e 's/(typedef pthread_once_t  once_flag;\n#  define ONCE_FLAG_INIT PTHREAD_ONCE_INIT\n)/#ifndef __once_flag_defined\n\$1#endif\n/' src/src/c11/threads.h && \
		perl -0pi -e 's/(void\ncall_once\(once_flag \*flag, void \(\*func\)\(void\)\)\n\{\n    pthread_once\(flag, func\);\n\}\n)/#ifndef __once_flag_defined\n\$1#endif\n/' src/src/c11/impl/threads_posix.c && \
		grep -q __once_flag_defined src/src/c11/threads.h && \
		grep -q __once_flag_defined src/src/c11/impl/threads_posix.c"
	# glvnd: the image's libGL/libEGL dispatch to libGLX_mesa/libEGL_mesa.
	SETUP="-Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm,kgsl -Dgallium-drivers=zink \
		-Dplatforms=x11,wayland -Dglx=dri -Degl=enabled -Dgbm=enabled -Dgles1=disabled \
		-Dgles2=enabled -Dopengl=true -Dllvm=disabled -Dglvnd=enabled \
		-Dbuildtype=release -Dprefix=/usr/local"
	PACK="./usr/local"
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
monado)
	# OpenXR runtime with the Quest 2 driver (monado/ in this repository).
	VER=$(cat "$HERE/patches/monado/BASE")
	FETCH="git clone -q https://gitlab.freedesktop.org/monado/monado.git src && git -C src checkout -q $VER"
	CONFIGURE="cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr/local \
		-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
		-DXRT_FEATURE_SERVICE=ON -DXRT_BUILD_DRIVER_QUEST2=ON -DXRT_HAVE_OPENCV=OFF -DXRT_HAVE_SDL2=OFF \
		-DXRT_HAVE_LIBUSB=OFF \
		-DXRT_FEATURE_WINDOW_PEEK=OFF -DBUILD_TESTING=OFF -DBUILD_DOC=OFF"
	PACK="./usr/local"
	# Driver sources live in this repository; copied in before every build.
	EXTRA_MOUNT="-v $HERE/monado:/qkx-monado:Z,ro"
	EXTRA="cp -r /qkx-monado/drivers/quest2 src/xrt/drivers/ && cp /qkx-monado/target_builder_quest2.c src/xrt/targets/common/"
	;;
hello_xr)
	# OpenXR SDK sample app, for testing the runtime (Vulkan).
	VER=${OPENXR_VERSION:-release-1.1.47}
	FETCH="git clone -q --depth 1 --branch $VER https://github.com/KhronosGroup/OpenXR-SDK-Source.git src"
	CONFIGURE="cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
		-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
		-DBUILD_TESTS=ON -DBUILD_CONFORMANCE_TESTS=OFF -DPRESENTATION_BACKEND=xlib -DBUILD_WITH_WAYLAND_HEADERS=OFF"
	PACK="./usr/local/bin/hello_xr"
	# Its bundled jinja2 predates current markupsafe: use the system one.
	EXTRA="rm -rf external/python/jinja2 external/python/markupsafe"
	;;
*) echo "unknown component $NAME"; exit 1 ;;
esac

# Builder images are created on first use.
if ! podman image exists "$BUILDER"; then
	if [ "$BUILDER" = qkx-holo-builder ]; then
		podman image exists qkx-deckard-base ||
			{ echo "run tools/steamos-rootfs.sh first (imports Valve's base image)"; exit 1; }
		podman build --platform linux/arm64 -t qkx-holo-builder -f "$HERE/tools/builder/Containerfile.holo" "$HERE/tools"
	else
		podman build --platform linux/arm64 -t qkx-builder "$HERE/tools/builder"
	fi
fi

PDIR=$HERE/patches/${NAME%-holo}
mkdir -p "$PDIR"
PSUM=$({ cat "$PDIR"/*.patch 2>/dev/null || true; } | sha256sum | cut -c1-16)
podman run --rm --platform linux/arm64 -e PSUM="$PSUM" \
	-e CCACHE_DIR=/ccache -v "$B/ccache:/ccache:Z" \
	-v "$HERE/out/src:/src:Z" -v "$PDIR:/patches:Z,ro" \
	${EXTRA_MOUNT:-} -v "$B/$NAME:/w:Z" -w /w "$BUILDER" sh -euc "
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
if [ -n \"${CONFIGURE:-}\" ]; then
	[ -f build/build.ninja ] || ${CONFIGURE:-true}
else
	[ -f build/build.ninja ] || CC='ccache gcc' CXX='ccache g++' meson setup build ${SETUP:-}
fi
ninja -C build -j\$(nproc)
rm -rf /w/inst && DESTDIR=/w/inst ninja -C build install >/dev/null
tar -C /w/inst -czf /w/out.tar.gz $PACK
"
mv "$B/$NAME/out.tar.gz" "$B/$NAME.tar.gz"
ls -l "$B/$NAME.tar.gz"
