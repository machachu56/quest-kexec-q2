#!/usr/bin/env bash
# Build GTK+ 2.24 for the SteamOS chroot. The Frame's Steam client
# (steamui.so, vgui2_s.so) links libgtk-x11-2.0, but Valve's public deckard
# repositories do not carry GTK 2 (Arch dropped it). Built against the same
# Valve base so library versions match the image.
#
# Incremental: a persistent container (qkx-gtk2-build) keeps the installed
# build dependencies, and the source/build tree lives in out/build/gtk2, so a
# retry resumes where it failed. Output: out/steamos/gtk2.tar.gz.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$HERE/out/steamos
B=$HERE/out/build/gtk2
E=https://steamdeck-packages.steamos.cloud/archlinux-deckard/archlinux/main/extra/os/aarch64
C=qkx-gtk2-build
mkdir -p "$OUT" "$B"

if ! podman container exists "$C"; then
	# Packages present in Valve's extra/ directory but missing from its database.
	curl -fsSL "$E/" > "$OUT/extra-index.html"
	URLS=""
	for n in at-spi2-core gsettings-desktop-schemas gdk-pixbuf2 libxinerama libxcomposite libxdamage; do
		f=$(grep -oE "href=\"$n-[0-9:%][^\"]*\.pkg\.tar\.zst\"" "$OUT/extra-index.html" |
			grep -v debug | sed 's/href=//;s/"//g' | sort -V | tail -1)
		URLS="$URLS $E/$f"
	done
	podman run -d --name "$C" --platform linux/arm64 \
		-v "$HERE/tools/steamos/pacman.conf:/etc/pacman.conf:Z,ro" \
		-v "$B:/b:Z" -v "$OUT:/out:Z" qkx-deckard-base sleep infinity >/dev/null
	podman exec -e URLS="$URLS" "$C" sh -euc '
pacman -Sy --noconfirm --needed base-devel pango cairo libxcursor libxrandr libxi \
	libxext libxrender libx11 shared-mime-info python perl glib2-devel >/dev/null
pacman -U --noconfirm --needed $URLS >/dev/null'
fi
podman start "$C" >/dev/null

podman exec "$C" sh -euc '
cd /b
if [ ! -d gtk+-2.24.33 ]; then
	curl -fsSL -o gtk.tar.xz https://download.gnome.org/sources/gtk+/2.24/gtk+-2.24.33.tar.xz
	echo "ac2ac757f5942d318a311a54b0c80b5ef295f299c2a73c632f6bfb1ff49cc6da  gtk.tar.xz" | sha256sum -c -
	tar xJf gtk.tar.xz
fi
cd gtk+-2.24.33
if [ ! -f config.status ]; then
	# Modern compilers reject GTK2-era implicit declarations; relax those.
	CFLAGS="-O2 -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=int-conversion" \
	./configure --prefix=/usr --sysconfdir=/etc --disable-gtk-doc --disable-man \
		--disable-introspection --disable-cups --disable-papi --disable-glibtest >/dev/null
fi
# Libraries and modules only: the bundled tests/demos do not build with modern GCC.
# (SUBDIRS= on the command line would propagate into every sub-make.)
for d in gdk gtk modules; do make -C $d -j"$(nproc)" >/dev/null; done
rm -rf /tmp/inst
for d in gdk gtk modules; do make -C $d install DESTDIR=/tmp/inst >/dev/null; done
make install-pkgconfigDATA DESTDIR=/tmp/inst >/dev/null 2>&1 || true
rm -rf /tmp/inst/usr/share/gtk-doc /tmp/inst/usr/share/locale
tar -C /tmp/inst -czf /out/gtk2.tar.gz .
'
ls -l "$OUT/gtk2.tar.gz"
