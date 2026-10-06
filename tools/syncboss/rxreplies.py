#!/usr/bin/env python3
# Decode the SyncBoss reply trace (kprobe qkx_rx on the miscfifo
# rx_packet_handler, seq != 0): type, seq, payload. Usage: rxreplies.py trace [type]
import re, struct, sys
want = int(sys.argv[2]) if len(sys.argv) > 2 else None
for line in open(sys.argv[1]):
    m = re.search(r"\s([\d.]+): qkx_rx: .* type=(\d+) seq=(\d+) len=(\d+) (.*)", line)
    if not m:
        continue
    t, seq, n = int(m.group(2)), int(m.group(3)), int(m.group(4))
    if want is not None and t != want:
        continue
    words = [int(x, 16) for x in re.findall(r"w\d+=0x([0-9a-f]+)", m.group(5))]
    raw = b"".join(struct.pack("<Q", w) for w in words)[3:3 + n]
    print("%s type=%d seq=%d len=%d %s" % (m.group(1), t, seq, n, raw.hex()))
