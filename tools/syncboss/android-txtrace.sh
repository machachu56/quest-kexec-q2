#!/system/bin/sh
# Log every packet Android sends to the SyncBoss MCU (run as root on the
# headset under Android). Kprobe on the driver's queue_tx_packet(devdata, buf,
# count): records count and the first 128 bytes of buf. Read-only tracing.
#   sh android-txtrace.sh start | stop | dump
T=/sys/kernel/tracing
[ -d $T/events ] || T=/sys/kernel/debug/tracing
case "$1" in
start)
	grep -w queue_tx_packet /proc/kallsyms || { echo "queue_tx_packet not in kallsyms"; exit 1; }
	echo 0 > $T/tracing_on
	echo > $T/trace
	echo 8192 > $T/buffer_size_kb
	a="len=%x2:u64"
	i=0; while [ $i -lt 16 ]; do a="$a b$i=+$((i * 8))(%x1):x64"; i=$((i + 1)); done
	echo "-:qkx_tx" >> $T/kprobe_events 2>/dev/null
	echo "p:qkx_tx queue_tx_packet $a" >> $T/kprobe_events || exit 1
	echo 1 > $T/events/kprobes/qkx_tx/enable
	echo 1 > $T/tracing_on
	echo "tracing; holders of /dev/syncboss0:"
	for p in /proc/[0-9]*; do
		ls -l $p/fd 2>/dev/null | grep -q syncboss0 && echo "  ${p#/proc/} $(cat $p/cmdline | tr '\0' ' ')"
	done
	;;
stop)
	echo 0 > $T/events/kprobes/qkx_tx/enable
	echo "-:qkx_tx" >> $T/kprobe_events
	;;
dump)
	cat $T/trace
	;;
esac
