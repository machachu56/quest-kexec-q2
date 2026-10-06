#!/usr/bin/env python3
# Replay the SyncBoss start-up commands recorded from Android's sensors HAL
# (android-start.json: [time s, type, payload hex]) on Linux, then capture
# for N seconds and print a histogram of received packet types.
#   sbstart.py [seconds] [start.json]
import os, sys, time, json, select, fcntl, array, collections
SEQ_ALLOC = 0x80010A02
dur = float(sys.argv[1]) if len(sys.argv) > 1 else 20
seqf = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "android-start.json")
open("/sys/devices/virtual/misc/syncboss0/spi/control/transaction_length", "w").write("512")
main = os.open("/dev/syncboss0", os.O_RDWR)
fds = {os.open("/dev/syncboss_stream0", os.O_RDONLY | os.O_NONBLOCK): "str",
       os.open("/dev/syncboss_control0", os.O_RDONLY | os.O_NONBLOCK): "ctl"}
time.sleep(0.5)
cnt = collections.Counter(); ex = {}

def drain(timeout):
    r, _, _ = select.select(list(fds), [], [], timeout)
    for fd in r:
        try: b = os.read(fd, 4096)
        except BlockingIOError: continue
        p = b[b[1]:]; k = (fds[fd], p[0])
        cnt[k] += 1; ex.setdefault(k, p[:3 + p[2]].hex()[:120])

t0 = time.time()
for at, typ, payload in json.load(open(seqf)):
    while time.time() - t0 < at: drain(0.002)
    seq = 0
    if typ == 2 or payload:
        a = array.array("B", [0]); fcntl.ioctl(main, SEQ_ALLOC, a, True); seq = a[0]
    data = bytes.fromhex(payload)
    os.write(main, bytes([typ, seq, len(data)]) + data)
t1 = time.time()
while time.time() - t1 < dur: drain(0.2)
for k, v in cnt.most_common(): print(k, v, ex[k])
