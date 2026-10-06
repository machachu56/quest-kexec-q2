#!/usr/bin/env python3
# Decode android-txtrace.sh output into SyncBoss packets.
#   txdecode.py trace [--hist] [--type N] [--first N]
import re, struct, sys, collections
pkts = []
for line in open(sys.argv[1]):
    m = re.search(r"\s([\d.]+): qkx_tx: .* len=(\d+) (.*)", line)
    if not m: continue
    words = [int(x, 16) for x in re.findall(r"b\d+=0x([0-9a-f]+)", m.group(3))]
    raw = b"".join(struct.pack("<Q", w) for w in words)[:int(m.group(2))]
    pkts.append((float(m.group(1)), raw))
args = sys.argv[2:]
if "--hist" in args:
    c = collections.Counter(p[0] for _, p in pkts)
    first = {}
    for t, p in pkts: first.setdefault(p[0], t)
    for k, v in sorted(c.items(), key=lambda kv: first[kv[0]]):
        print("type %3d  n=%5d  first=%.3f" % (k, v, first[k] - pkts[0][0]))
else:
    want = int(args[args.index("--type") + 1]) if "--type" in args else None
    n = int(args[args.index("--first") + 1]) if "--first" in args else 40
    for t, p in pkts:
        if want is not None and p[0] != want: continue
        print("%.3f type=%d seq=%d len=%d %s" % (t - pkts[0][0], p[0], p[1], p[2], p[3:3 + min(p[2], 125)].hex()))
        n -= 1
        if n == 0: break
