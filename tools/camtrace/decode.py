#!/usr/bin/env python3
# Decode qkx_camtrace logs (module/qkx_camtrace.c) into a merged timeline.
#   decode.py LOG [LOG...] [--summary] [--limit N]
import struct, sys, collections

REC = struct.Struct("<IIQIIIIqQ")
PHASES = ["PRE", "POST", "PACKET", "CMDBUF", "RAW_PRE", "RAW_POST"]
NODES = {0: "req_mgr", 1: "sync", 128: "cpas", 129: "isp", 130: "isp-fp",
         131: "csiphy0", 132: "csiphy1", 133: "csiphy2", 134: "csiphy3",
         135: "csiphy4", 136: "csiphy5", 137: "sensor0", 138: "sensor1",
         139: "sensor2", 140: "sensor3"}
COMMON = {0x100 + i: n for i, n in enumerate(
    ["?", "QUERY_CAP", "ACQUIRE_DEV", "START_DEV", "STOP_DEV", "CONFIG_DEV",
     "RELEASE_DEV", "SD_SHUTDOWN", "FLUSH_REQ", "SET_STREAM_MODE", "STREAM_MODE_CMD"])}
COMMON.update({0x200 + i: n for i, n in enumerate(["?", "ACQUIRE_HW", "RELEASE_HW", "DUMP_REQ"])})
# cam_req_mgr.h: CAM_REQ_MGR_* = CAM_COMMON_OPCODE_MAX (0x10b) + n
REQMGR = {0x10c + i: n for i, n in enumerate(
    ["CREATE_DEV_NODES", "CREATE_SESSION", "DESTROY_SESSION", "LINK", "UNLINK",
     "SCHED_REQ", "FLUSH_REQ", "SYNC_MODE", "ALLOC_BUF", "MAP_BUF", "RELEASE_BUF",
     "CACHE_OPS", "LINK_CONTROL", "LINK_V2", "REQUEST_DUMP"])}
COMMON[0x10c] = "SENSOR_PROBE"  # cam_sensor.h, on sensor nodes
RAW = {0x4020565a: "SUBSCRIBE_EVENT", 0x4020565b: "UNSUBSCRIBE_EVENT", 0x80885659: "DQEVENT"}


def read(path):
    d = open(path, "rb").read()
    i = 0
    while i + REC.size <= len(d):
        magic, ln, ts, pid, minor, cmd, phase, ret, aux = REC.unpack_from(d, i)
        if magic != 0x31544351:
            print(f"{path}: bad magic at {i}", file=sys.stderr)
            break
        yield dict(ts=ts, pid=pid, minor=minor, cmd=cmd, phase=PHASES[phase] if phase < 6 else phase,
                   ret=ret, aux=aux, data=d[i + REC.size:i + REC.size + ln], src=path)
        i += REC.size + ln


def opname(r):
    if len(r["data"]) >= 24 and r["phase"] in ("PRE", "POST"):
        op = struct.unpack_from("<I", r["data"])[0]
        return (REQMGR if r["minor"] == 0 else COMMON).get(op, hex(op))
    return RAW.get(r["cmd"], "ioctl_%08x" % r["cmd"])


def main():
    paths = [a for a in sys.argv[1:] if not a.startswith("--")]
    limit = int(sys.argv[sys.argv.index("--limit") + 1]) if "--limit" in sys.argv else 10 ** 9
    recs = sorted((r for p in paths for r in read(p)), key=lambda r: r["ts"])
    t0 = recs[0]["ts"] if recs else 0
    if "--summary" in sys.argv:
        c = collections.Counter((NODES.get(r["minor"], r["minor"]), r["phase"], opname(r)) for r in recs)
        for k, v in sorted(c.items(), key=lambda kv: -kv[1]):
            print("%6d  %-9s %-8s %s" % (v, *k))
        return
    for r in recs[:limit]:
        print("%9.4f %-8s %-8s %-16s ret=%-6d aux=%-6x len=%d" % (
            (r["ts"] - t0) / 1e9, NODES.get(r["minor"], r["minor"]), r["phase"], opname(r),
            r["ret"], r["aux"], len(r["data"])))


if __name__ == "__main__":
    main()
