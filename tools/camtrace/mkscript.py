#!/usr/bin/env python3
# Compile a qkx_camtrace recording (+ SyncBoss TX trace) into the compact
# camera start-up script executed by the Monado Quest 2 driver
# (monado/drivers/quest2/q2_cameras.c). Same selection and ordering as
# replay.py, which stays the reference implementation.
#
#   mkscript.py <dir> [--run run4] [-o q2-cameras.bin]
#
# Format (little endian): "Q2CS", u32 version=1, u32 op count, then ops:
#   u8 kind, u32 delay_us (since the previous op), payload:
#   1 CAMCTL : u16 minor, u32 op, u32 size, u32 htype, u64 handle,
#              blob payload, u32 nptr, nptr x (u32 offset, blob data),
#              blob recorded_post, u32 nptr_post, nptr_post x (u32 offset, blob)
#   2 RAW    : u16 minor, u32 cmd, blob data
#   3 MEM    : u32 handle, u64 offset, u8 translate, blob data
#   4 SB     : blob packet (type, seq, len, payload)
# where blob = u32 length + bytes.
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import replay  # noqa: E402  (reuses its readers and constants)

def blob(b):
    return struct.pack("<I", len(b)) + bytes(b)


def main():
    d = sys.argv[1]
    run = sys.argv[sys.argv.index("--run") + 1] if "--run" in sys.argv else "run4"
    out = sys.argv[sys.argv.index("-o") + 1] if "-o" in sys.argv else "q2-cameras.bin"

    recs = list(replay.read_log(os.path.join(d, run + "-cam.bin")))
    t0 = min(r["ts"] for r in recs if r["phase"] == "RAW_PRE" and r["minor"] == 0)
    t_last = max(r["ts"] for r in recs if r["phase"] == "PRE" and replay.op_of(r) == replay.OP_START_DEV)
    recs = [r for r in recs if t0 <= r["ts"] <= t_last + 10 ** 8]
    sb = [r for r in replay.read_syncboss(os.path.join(d, run + "-syncboss-tx.trace"))
          if t0 - 10 ** 8 <= r["ts"] <= t_last + 2 * 10 ** 9]
    timeline = sorted(recs + sb, key=lambda r: r["ts"])

    # Pair each PRE with its POST and pointer records (same node and ioctl).
    ops, last_ts, i = [], timeline[0]["ts"], 0
    pending_ptrs = []
    posts = {}
    for idx, r in enumerate(timeline):
        if r["phase"] == "POST":
            posts.setdefault((r["minor"], r["cmd"]), []).append(idx)

    def delay(r):
        nonlocal last_ts
        dt = max(0, (r["ts"] - last_ts) // 1000)
        last_ts = max(last_ts, r["ts"])
        return dt

    for idx, r in enumerate(timeline):
        ph = r["phase"]
        if ph == "SB":
            if r["data"][0] in replay.SB_SKIP:
                continue
            ops.append(struct.pack("<BI", 4, delay(r)) + blob(r["data"]))
        elif ph == "PTR":
            pending_ptrs.append(r)
        elif ph in ("PACKET", "CMDBUF"):
            ops.append(struct.pack("<BIIQB", 3, delay(r), r["ret"] & 0xffffffff, r["aux"], ph == "PACKET")
                       + blob(r["data"]))
        elif ph == "RAW_PRE":
            if r["cmd"] == replay.DQEVENT or not r["data"]:
                continue
            ops.append(struct.pack("<BIHI", 2, delay(r), r["minor"], r["cmd"]) + blob(r["data"]))
        elif ph == "PRE":
            if replay.op_of(r) == replay.OP_STREAM_MODE_CMD:
                pending_ptrs = []
                continue
            op, size, htype, _, handle = struct.unpack_from("<IIIIQ", r["data"])
            # Recorded result: the next POST of this node/ioctl, and its pointers.
            post, post_ptrs = b"", []
            for pidx in posts.get((r["minor"], r["cmd"]), []):
                if pidx > idx:
                    post = timeline[pidx]["data"][24:]
                    j = pidx - 1
                    while j > idx and timeline[j]["phase"] == "PTR_POST":
                        post_ptrs.insert(0, timeline[j])
                        j -= 1
                    break
            body = struct.pack("<HIIIQ", r["minor"], op, size, htype, handle) + blob(r["data"][24:])
            body += struct.pack("<I", len(pending_ptrs))
            for p in pending_ptrs:
                body += struct.pack("<I", p["ret"]) + blob(p["data"])
            body += blob(post) + struct.pack("<I", len(post_ptrs))
            for p in post_ptrs:
                body += struct.pack("<I", p["ret"]) + blob(p["data"])
            ops.append(struct.pack("<BI", 1, delay(r)) + body)
            pending_ptrs = []

    with open(out, "wb") as f:
        f.write(b"Q2CS" + struct.pack("<II", 1, len(ops)) + b"".join(ops))
    kinds = {}
    for o in ops:
        kinds[o[0]] = kinds.get(o[0], 0) + 1
    print("%s: %d ops %s, %d bytes" % (out, len(ops), kinds, os.path.getsize(out)))


if __name__ == "__main__":
    main()
