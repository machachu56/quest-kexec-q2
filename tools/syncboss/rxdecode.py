#!/usr/bin/env python3
# Split a raw dump of /dev/syncboss_stream0 (uapi header + syncboss_data
# packets back to back) into packets.  rxdecode.py dump [--hist] [--type N] [--first N]
import sys, collections
d = open(sys.argv[1], "rb").read()
pkts = []; i = 0
while i + 3 < len(d):
    hl = d[i + 1]
    if hl < 2 or i + hl + 3 > len(d): break
    t, seq, n = d[i + hl], d[i + hl + 1], d[i + hl + 2]
    pkts.append((t, seq, d[i + hl + 3:i + hl + 3 + n]))
    i += hl + 3 + n
args = sys.argv[2:]
if "--hist" in args:
    c = collections.Counter((t, len(p)) for t, _, p in pkts)
    for k, v in c.most_common(): print("type %3d len %3d  n=%d" % (k[0], k[1], v))
    print("parsed", len(pkts), "packets, stopped at", i, "of", len(d))
else:
    want = int(args[args.index("--type") + 1]) if "--type" in args else None
    n = int(args[args.index("--first") + 1]) if "--first" in args else 20
    for t, seq, p in pkts:
        if want is not None and t != want: continue
        print(t, seq, p.hex()); n -= 1
        if not n: break
