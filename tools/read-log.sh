#!/usr/bin/env bash
# After the device is back in Android, print the log the target kernel kept
# in RAM across a warm reset, plus any Android pstore panic record.
set -uo pipefail
case "$(adb shell getprop ro.product.device | tr -d '\r')" in
	hollywood) LOG_PHYS=0x9ba40000 ;;
	*) LOG_PHYS=0x9ba80000 ;;
esac
adb shell "su -c 'insmod /data/local/tmp/qkx_marker.ko log_phys=$LOG_PHYS; rmmod marker_read; dmesg'" |
	awk '/qkx_marker_read:/{f=1} f'
adb shell 'su -c "cat /sys/fs/pstore/dmesg-ramoops-0 2>/dev/null"' | grep -a 'quest_kexec\|Kernel panic' | tail -5
# On Quest 2 the retained log (0x9ba80000) lies inside Android's pmsg zone,
# which logd overwrites soon after boot. Android snapshots the old zone into
# pmsg-ramoops-0 first, so recover the log (header magic "LXKQ") from there.
tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT
adb exec-out 'su -c "cat /sys/fs/pstore/pmsg-ramoops-0 2>/dev/null"' > "$tmp"
python3 - "$tmp" <<'EOF'
import struct, sys
data = open(sys.argv[1], 'rb').read()
i = data.find(struct.pack('<I', 0x514b584c))
if i < 0:
    sys.exit(print('pmsg snapshot: no retained target log'))
pos, wraps = struct.unpack_from('<II', data, i + 4)
body = data[i + 16:i + 0x10000]
text = body[pos:] + body[:pos] if wraps else body[:pos]
print(f'pmsg snapshot: retained target log, write_pos={pos} wraps={wraps}')
print(text.decode('ascii', 'replace').rstrip('\0'))
EOF
