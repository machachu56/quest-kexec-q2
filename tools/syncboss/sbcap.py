#!/usr/bin/env python3
# Capture SyncBoss packets for N seconds with the headset IMU enabled
# (command 110, IMU 0) and print a histogram of packet types.
# Needs /sys/devices/virtual/misc/syncboss0/spi/control/transaction_length = 512.
# Non-IMU/display packets are logged to /root/sbcap.log.
import os, time, collections, select, fcntl, array, sys
dur = float(sys.argv[1])
main = os.open("/dev/syncboss0", os.O_RDWR)
fds = {os.open("/dev/syncboss_stream0", os.O_RDONLY | os.O_NONBLOCK): "str",
       os.open("/dev/syncboss_control0", os.O_RDONLY | os.O_NONBLOCK): "ctl"}
time.sleep(0.5)
seq = array.array("B", [0]); fcntl.ioctl(main, 0x80010A02, seq, True)
os.write(main, bytes([110, seq[0], 1, 0]))
cnt = collections.Counter(); ex = collections.defaultdict(list); t0 = time.time(); first = {}
log = open("/root/sbcap.log", "w")
while time.time() - t0 < dur:
    r, _, _ = select.select(list(fds), [], [], 0.5)
    for fd in r:
        try: b = os.read(fd, 4096)
        except BlockingIOError: continue
        p = b[b[1]:]; k = (fds[fd], p[0], p[2])
        cnt[k] += 1
        first.setdefault(k, round(time.time() - t0, 1))
        if p[0] not in (80, 85): log.write("%.3f %s %s\n" % (time.time() - t0, fds[fd], p[:3 + p[2]].hex()))
        if len(ex[k]) < 3: ex[k].append(p[:3 + p[2]].hex())
for k, v in cnt.most_common(): print(k, v, "first@", first[k], ex[k][0][:100])
