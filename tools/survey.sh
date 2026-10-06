#!/usr/bin/env bash
# Read-only porting survey for a new Quest headset (written for Quest 2).
# Collects everything the loader, prepare.py and run.sh assume about the
# board, then checks those assumptions host-side. Writes nothing on device.
# Usage: survey.sh <output-dir>
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
O=$1; mkdir -p "$O"
su() { adb shell "su -c '$1'" | tr -d '\r'; }

adb shell 'su -c true' >/dev/null 2>&1 || { echo "need adb with working su"; exit 1; }

# Identity and kernel build. The loader module must match release exactly.
{
	for p in ro.product.device ro.product.model ro.build.fingerprint \
		ro.build.version.incremental ro.boot.hardware ro.board.platform; do
		echo "$p=$(adb shell getprop $p | tr -d '\r')"
	done
	echo "uname=$(adb shell uname -r | tr -d '\r')"
	echo "version=$(su 'cat /proc/version')"
	echo "dt_model=$(su 'cat /proc/device-tree/model' | tr -d '\0')"
	echo "dt_compatible=$(su 'cat /proc/device-tree/compatible' | tr '\0' ' ')"
	echo "cpus=$(su 'cat /sys/devices/system/cpu/possible')"
	echo "memtotal=$(su 'grep MemTotal /proc/meminfo')"
} > "$O/identity"

su 'cat /proc/iomem' > "$O/iomem"
su 'cat /proc/cmdline' > "$O/cmdline"
adb exec-out 'su -c "zcat /proc/config.gz"' > "$O/android.config"
su 'ls /proc/device-tree/reserved-memory' > "$O/reserved-memory"
su 'ls /sys/bus/platform/devices' > "$O/platform-devices"
su 'for d in /sys/bus/spi/devices/*; do echo "$(basename $d) $(cat $d/modalias 2>/dev/null) driver=$(basename $(readlink $d/driver) 2>/dev/null)"; done' > "$O/spi-devices"
su 'ls /sys/class/udc' > "$O/udc"
su 'cat /proc/kallsyms' > "$O/kallsyms"
su 'lsmod' > "$O/lsmod"

# Runtime DT, same method as capture.sh (boot FDT is not usable for kexec).
if command -v dtc >/dev/null; then
	tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
	adb exec-out 'su -c "tar -C /proc/device-tree -cf - ."' |
		tar --warning=no-timestamp -C "$tmp" -xf -
	dtc -q -I fs -O dtb "$tmp" -o "$O/runtime.dtb"
else
	echo "dtc missing: runtime.dtb not captured" >&2
fi

# ---- Host-side checks of every board assumption ---------------------------
R=$O/report
: > "$R"
ok()   { echo "OK    $*" | tee -a "$R"; }
bad()  { echo "FAIL  $*" | tee -a "$R"; }
note() { echo "NOTE  $*" | tee -a "$R"; }

grep -q 'qcom,kona' "$O/identity" && ok "SoC is qcom,kona" || bad "SoC is not qcom,kona (loader refuses)"

# Platform devices loader.c and usb_log.c look up by name.
for d in 3d00000.qcom,kgsl-3d0 17c10000.qcom,wdt a600000.dwc3 \
	c300000.qcom,qmp-aop 18200000.rsc:rpmh-regulator-mmcxlvl; do
	grep -qx "$d" "$O/platform-devices" && ok "device $d" || bad "device $d missing"
done
grep -q '^spi0.0 ' "$O/spi-devices" && ok "spi0.0: $(grep '^spi0.0 ' "$O/spi-devices")" ||
	bad "spi0.0 missing (suspend_syncboss=1 will fail); see spi-devices"
grep -qx 'a600000.dwc3' "$O/udc" && ok "UDC a600000.dwc3" || bad "UDC is $(tr '\n' ' ' < "$O/udc")"

# Symbols resolved through kallsyms_lookup_name at runtime.
for s in kallsyms_lookup_name adreno_pm_resume adreno_pm_suspend __boot_cpu_mode \
	bus_find_device_by_name class_find_device cpu_down cpu_hotplug_enable \
	cpus_are_stuck_in_kernel cpu_up dwc3_gadget_suspend enable_nonboot_cpus \
	events_check_enabled force_warm_reboot freeze_processes freeze_secondary_cpus \
	hyp_assign_table ion_hyp_unassign_sg_from_flags kernel_restart_prepare \
	kexec_in_progress kgsl_driver kgsl_secure_guard_page lock_system_sleep \
	machine_restart machine_shutdown migrate_to_reboot_cpu platform_bus_type \
	pm_abort_suspend qmp_shutdown register_console rpmh_flush scm_call2 scm_io_read \
	scm_io_write scm_is_call_available send_irq set_cpus_allowed_ptr spi_bus_type \
	thaw_processes udc_class udc_lock unlock_system_sleep walk_system_ram_res \
	wdog_disable_set; do
	grep -qE " $s(\s|$)" "$O/kallsyms" || bad "symbol $s missing"
done
awk '{print $1}' "$O/kallsyms" | grep -qv '^0*$' || bad "kallsyms addresses hidden (kptr_restrict)"

# Kernel config the loader and transition.S are built for.
for c in CONFIG_ARM64_VA_BITS=39 CONFIG_ARM64_4K_PAGES=y CONFIG_KPROBES=y CONFIG_MODULES=y; do
	grep -qx "$c" "$O/android.config" && ok "$c" || bad "$c not set"
done
grep -qx 'CONFIG_MODULE_SIG_FORCE=y' "$O/android.config" &&
	bad "CONFIG_MODULE_SIG_FORCE=y: unsigned modules refused" || ok "module signatures not enforced"

# Staging window 0x90000000-0x93400000 must be plain System RAM.
python3 -c "
import sys; sys.path.insert(0, '$HERE')
from prepare import check_ram
try:
    check_ram(open('$O/iomem').read(), 0x90000000, 0x3400000)
    print('OK    staging window 0x90000000-0x93400000 is free System RAM')
except ValueError as e:
    print('FAIL  staging window:', e)
" | tee -a "$R"

note "secure carveouts: $(grep -c . "$O/reserved-memory") reserved-memory nodes"
echo "survey written to $O (report: $R)"
