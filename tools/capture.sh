#!/usr/bin/env bash
# Capture the device-specific inputs needed by prepare.py (read-only on device).
# Usage: capture.sh <output-dir>
set -euo pipefail
O=$1; mkdir -p "$O"
su() { adb shell "su -c \"$1\""; }
# /sys/firmware/fdt is the original boot FDT. Kexec needs the kernel's
# runtime tree after overlays/fixups; using the boot FDT caused a cold reset.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
adb exec-out 'su -c "tar -C /proc/device-tree -cf - ."' |
    tar --warning=no-timestamp -C "$tmp" -xf -
dtc -q -I fs -O dtb "$tmp" -o "$O/runtime-unordered.dtb"
# dtc -I fs shuffles node order; kgsl (GMU lookup) and /cpus numbering depend
# on it. Restore the boot FDT's order around the runtime tree's content.
adb exec-out 'su -c "cat /sys/firmware/fdt"' > "$O/boot.fdt"
python3 "$(dirname "$0")/dt-reorder.py" "$O/runtime-unordered.dtb" "$O/boot.fdt" "$O/runtime.dtb"
su 'cat /proc/iomem' > "$O/iomem"
# Where Android placed dynamic reserved-memory regions (pinned by prep.sh).
su 'dmesg' | grep -E 'Reserved memory: created|reserved mem: initialized node' > "$O/android-reserved.txt"
su 'cat /proc/cmdline' > "$O/cmdline"
adb exec-out 'su -c "zcat /proc/config.gz"' > "$O/android.config"
adb shell uname -r > "$O/android-release"
echo "captured into $O: runtime.dtb iomem cmdline android.config android-release"
