#!/usr/bin/env bash
# Jump to a payload and report the outcome without manual steps:
# USB gadget up -> done; back in Android -> wait for root (re-activate it),
# then print the PMIC power-off reason and the retained target log.
# Usage: tools/q2-jump.sh <payload-dir> [extra loader params...]
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
"$HERE/run.sh" "$@" watchdog_recovery=1 2>&1 | tail -2
for i in $(seq 90); do
	lsusb | grep -q 1d6b:0105 && { echo "RESULT: target up after $((i*2)) s"; exit 0; }
	lsusb | grep -q '2833:' && { echo "RESULT: back in Android after $((i*2)) s"; break; }
	sleep 2
done
lsusb | grep -q '2833:' || { echo 'RESULT: no USB device (hung?); do not hold power yet'; exit 2; }
echo 're-activate root on the headset...'
until adb shell 'su -c true' >/dev/null 2>&1; do sleep 3; done
adb shell 'su -c "dmesg | grep -m1 \"SID0: Power-off reason\""' | sed 's/.*reason: /power-off: /'
"$HERE/read-log.sh" 2>&1 | grep -aE 'QKX|qkx-init|qkx-linux|ENODEV|panic|no retained|retained log' | sed 's/.*qkxlog: //' | tail -25
exit 1
