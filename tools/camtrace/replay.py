#!/usr/bin/env python3
# Replay the Quest 2 tracking-camera start-up recorded on Android (qkx_camtrace
# logs + the SyncBoss TX trace) on Linux, then run the frame loop and save
# frames from the four OV7251 cameras as PGM images.
#
#   replay.py <log-dir> [seconds] [--frames N] [--out DIR]
#
# <log-dir> holds <run>-cam.bin (one qkx_camtrace log: all nodes, copy_any=1)
# and <run>-syncboss-tx.trace (android-txtrace.sh); --run selects the prefix. Kernel-generated values (session, device, link,
# memory and IOMMU handles) are learnt from the live results and substituted
# into later arguments and packets. Android maps gralloc buffers for the images
# (MAP_BUF); here they are allocated by the camera driver instead.
import ctypes, fcntl, mmap, os, re, struct, sys, time, array

REC = struct.Struct("<IIQIIIIqQ")
PHASES = ["PRE", "POST", "PACKET", "CMDBUF", "RAW_PRE", "RAW_POST", "PTR", "PTR_POST"]
VIDIOC_CAM_CONTROL = 0xC01856C0
DQEVENT = 0x80885659
OP_QUERY_CAP, OP_ACQUIRE_DEV, OP_START_DEV, OP_CONFIG_DEV = 0x101, 0x102, 0x103, 0x105
OP_SET_STREAM_MODE, OP_STREAM_MODE_CMD = 0x109, 0x10A
OP_ALLOC, OP_MAP, OP_RELEASE = 0x114, 0x115, 0x116
ISP_MINOR = 129
IMAGE_LEN = 512 * 1024      # 800 x 481 RAW10 = 384800 bytes
FRAME_W, FRAME_H, FRAME_STRIDE = 640, 480, 800
SB_SEQ_ALLOC = 0x80010A02
SB_SEQ_RELEASE = 0x40010A03
SB_SKIP = {90, 203, 204}    # shutdown-time commands that crash the MCU


def log(*a):
    print("replay:", *a, flush=True)


def read_log(path):
    d = open(path, "rb").read()
    i = 0
    while i + REC.size <= len(d):
        magic, ln, ts, pid, minor, cmd, phase, ret, aux = REC.unpack_from(d, i)
        if magic != 0x31544351:
            break
        yield dict(ts=ts, minor=minor, cmd=cmd, phase=PHASES[phase], ret=ret, aux=aux,
                   data=d[i + REC.size:i + REC.size + ln], src=os.path.basename(path))
        i += REC.size + ln


def read_syncboss(path):
    for line in open(path):
        m = re.search(r"\s([\d.]+): qkx_tx: .* len=(\d+) (.*)", line)
        if not m:
            continue
        words = [int(x, 16) for x in re.findall(r"b\d+=0x([0-9a-f]+)", m.group(3))]
        raw = b"".join(struct.pack("<Q", w) for w in words)[:int(m.group(2))]
        yield dict(ts=int(float(m.group(1)) * 1e9), phase="SB", data=raw, src="syncboss")


def op_of(r):
    return struct.unpack_from("<I", r["data"])[0] if len(r["data"]) >= 24 else None


class Replayer:
    def __init__(self):
        self.map = {}            # recorded handle (u32) -> live handle
        self.fds = {}            # recorded minor -> fd
        self.bufs = {}           # live mem handle -> (fd, mmap, len)
        self.pending = {}        # (src, minor, cmd) -> (recorded PRE data, live POST data)
        self.cams = []           # live (session, isp dev handle)
        self.sb = None
        self.ptrs = {}           # src -> [(payload offset, recorded bytes)] for the next PRE
        self.ptr_live = {}       # (src, minor, cmd) -> [(offset, recorded pre, live buffer)]
        self.done = []           # (minor, op, live payload) to undo on teardown

    # -- devices ---------------------------------------------------------
    def node(self, minor):
        if minor not in self.fds:
            path = "/dev/video%d" % minor if minor < 64 else "/dev/v4l-subdev%d" % (minor - 128)
            self.fds[minor] = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        return self.fds[minor]

    # -- handle translation ------------------------------------------------
    def translate(self, data):
        b = bytearray(data)
        for i in range(0, len(b) - 3, 4):
            v = struct.unpack_from("<I", b, i)[0]
            if v >= 0x100 and v in self.map:
                struct.pack_into("<I", b, i, self.map[v])
        return b

    def learn(self, rec_pre, rec_post, live_post, small_ok=False):
        # Session/device/link/memory handles carry a counter in their upper
        # 16 bits. Smaller outputs (lengths, counts) must not become
        # substitution keys, except IOMMU handles from capability queries.
        n = min(len(rec_pre), len(rec_post), len(live_post))
        for i in range(0, n - 3, 4):
            a, = struct.unpack_from("<I", rec_pre, i)
            b, = struct.unpack_from("<I", rec_post, i)
            c, = struct.unpack_from("<I", live_post, i)
            if a != b and (b >= 0x10000 or (small_ok and b >= 0x100)):
                self.map[b] = c

    # -- camera control ioctl ---------------------------------------------
    def cam_control(self, minor, ctl, payload):
        op, size, htype, _, handle = struct.unpack("<IIIIQ", ctl)
        buf = ctypes.create_string_buffer(bytes(payload), max(len(payload), size))
        if htype == 2:      # CAM_HANDLE_MEM_HANDLE: handle is a memory handle
            arg = self.map.get(handle & 0xffffffff, handle & 0xffffffff)
        else:
            arg = ctypes.addressof(buf)
        c = bytearray(struct.pack("<IIIIQ", op, size, htype, 0, arg))
        try:
            fcntl.ioctl(self.node(minor), VIDIOC_CAM_CONTROL, c, True)
            ret = 0
        except OSError as e:
            ret = -e.errno
        return ret, bytes(buf.raw[:len(payload)])

    def alloc(self, length, flags, mmu):
        p = bytearray(104)
        struct.pack_into("<QQ", p, 0, length, 4096)
        for i, h in enumerate(mmu):
            struct.pack_into("<i", p, 16 + 4 * i, h)
        struct.pack_into("<II", p, 80, len(mmu), flags)
        ret, out = self.cam_control(0, struct.pack("<IIIIQ", OP_ALLOC, 104, 0, 0, 0), p)
        if ret:
            raise OSError(-ret, "ALLOC_BUF")
        handle, fd = struct.unpack_from("<Ii", out, 88)
        self.bufs[handle] = (fd, mmap.mmap(fd, length), length)
        return handle, out

    # -- record handlers ----------------------------------------------------
    def do_pre(self, r):
        ctl, payload = r["data"][:24], r["data"][24:]
        op = op_of(r)
        key = (r["src"], r["minor"], r["cmd"])
        if op == OP_STREAM_MODE_CMD:
            return
        if r["minor"] == 0 and op == OP_MAP:
            # gralloc buffer on Android: allocate an image buffer instead.
            mmu = [self.map.get(h, h) for h in struct.unpack_from("<16i", payload, 0)[:struct.unpack_from("<I", payload, 64)[0]]]
            handle, _ = self.alloc(IMAGE_LEN, 0x1 | 0x10 | 0x80, mmu)
            live = bytearray(payload)
            struct.pack_into("<I", live, 80, handle)
            self.pending[key] = (payload, bytes(live))
            return
        live_in = self.translate(payload)
        # Structures the argument points at: give the kernel live copies.
        keep = []
        for off, data in self.ptrs.pop(r["src"], []):
            b = ctypes.create_string_buffer(bytes(self.translate(data)), len(data))
            struct.pack_into("<Q", live_in, off, ctypes.addressof(b))
            keep.append((off, data, b))
        ret, out = self.cam_control(r["minor"], ctl, live_in)
        self.ptr_live[key] = keep
        if ret == 0 and op in (OP_ACQUIRE_DEV, OP_START_DEV, 0x151):
            self.done.append((r["minor"], op, bytes(out)))
        if r["minor"] == 0 and op == OP_ALLOC and ret == 0:
            handle, fd = struct.unpack_from("<Ii", out, 88)
            length = struct.unpack_from("<Q", out, 0)[0]
            self.bufs[handle] = (fd, mmap.mmap(fd, length), length)
        if r["minor"] == ISP_MINOR and op == OP_ACQUIRE_DEV and ret == 0:
            sess, dev = struct.unpack_from("<ii", out, 0)
            self.cams.append([sess, dev])
        if ret:
            log("op %#x on minor %d failed: %d" % (op, r["minor"], ret))
        self.pending[key] = (payload, out)

    def do_post(self, r):
        key = (r["src"], r["minor"], r["cmd"])
        if key in self.pending and len(r["data"]) > 24:
            rec_pre, live_post = self.pending.pop(key)
            self.learn(rec_pre, r["data"][24:], live_post, small_ok=op_of(r) == OP_QUERY_CAP)

    def do_ptr(self, r):
        self.ptrs.setdefault(r["src"], []).append((r["ret"], r["data"]))

    def do_ptr_post(self, r):
        # Outputs written through pointers (e.g. IOMMU handles in caps).
        key = (r["src"], r["minor"], r["cmd"])
        for off, pre, b in self.ptr_live.get(key, []):
            if off == r["ret"]:
                self.learn(pre, r["data"], b.raw[:len(pre)], small_ok=True)

    def teardown(self):
        # Undo in reverse: stop started devices, release acquired ones.
        for minor, op, out in reversed(self.done):
            sess, dev = struct.unpack_from("<ii", out, 0)
            if op == OP_START_DEV:
                self.cam_control(minor, struct.pack("<IIIIQ", 0x104, 8, 1, 0, 0), struct.pack("<ii", sess, dev))
            elif op == OP_ACQUIRE_DEV:
                self.cam_control(minor, struct.pack("<IIIIQ", 0x106, 8, 1, 0, 0), struct.pack("<ii", sess, dev))
        self.done = []

    def do_raw(self, r):
        if r["cmd"] == DQEVENT or not r["data"]:
            return
        try:
            fcntl.ioctl(self.node(r["minor"]), r["cmd"], bytearray(r["data"]), True)
        except OSError as e:
            log("raw ioctl %#x on minor %d: %s" % (r["cmd"], r["minor"], e))

    def do_mem(self, r, translate):
        handle = self.map.get(r["ret"] & 0xffffffff)
        if handle is None or handle not in self.bufs:
            log("no live buffer for recorded handle %#x" % r["ret"])
            return
        _, mm, length = self.bufs[handle]
        data = self.translate(r["data"]) if translate else r["data"]
        off = r["aux"]
        if off + len(data) <= length:
            mm[off:off + len(data)] = data

    def do_syncboss(self, r):
        p = r["data"]
        if p[0] in SB_SKIP:
            return
        if self.sb is None:
            self.sb = os.open("/dev/syncboss0", os.O_RDWR)
        seq = 0
        if p[1]:
            a = array.array("B", [0])
            fcntl.ioctl(self.sb, SB_SEQ_ALLOC, a, True)
            seq = a[0]
        os.write(self.sb, bytes([p[0], seq, p[2]]) + p[3:3 + p[2]])
        if seq:
            fcntl.ioctl(self.sb, SB_SEQ_RELEASE, array.array("B", [seq]), False)

    # -- frames ---------------------------------------------------------------
    def frame_loop(self, seconds, nframes, outdir, images):
        os.makedirs(outdir, exist_ok=True)
        last = {i: [] for i in range(len(self.cams))}
        saved = {i: 0 for i in range(len(self.cams))}
        count = {i: 0 for i in range(len(self.cams))}
        t_end = time.time() + seconds
        while time.time() < t_end:
            for i, (sess, dev) in enumerate(self.cams):
                p = bytearray(1256)
                struct.pack_into("<iiI", p, 0, sess, dev, 3)
                struct.pack_into("<I", p, 16, len(last[i]))
                for j, iid in enumerate(last[i]):
                    struct.pack_into("<Q", p, 24 + 8 * j, iid)
                struct.pack_into("<II", p, 264, 50, 0)
                ret, out = self.cam_control(ISP_MINOR, struct.pack("<IIIIQ", OP_STREAM_MODE_CMD, 1256, 1, 0, 0), p)
                if ret:
                    if count[i] == 0 and ret != -110:
                        log("camera %d: STREAM_MODE_CMD %d" % (i, ret))
                    last[i] = []
                    continue
                n = struct.unpack_from("<I", out, 268)[0]
                last[i] = []
                for k in range(n):
                    iid, ts, sof, fnum = struct.unpack_from("<QQQq", out, 272 + 32 * k)
                    last[i].append(iid)
                    count[i] += 1
                    if saved[i] < nframes and (count[i] % 10) == 0:
                        self.save(images.get((i, iid)), "%s/cam%d_%03d.pgm" % (outdir, i, saved[i]))
                        saved[i] += 1
        log("frames per camera in %ds: %s" % (seconds, count))

    def save(self, handle, path):
        if handle is None or handle not in self.bufs:
            return
        _, mm, _ = self.bufs[handle]
        raw = mm[:FRAME_STRIDE * FRAME_H]
        img = bytearray(FRAME_W * FRAME_H)
        for y in range(FRAME_H):
            line = raw[y * FRAME_STRIDE:(y + 1) * FRAME_STRIDE]
            # MIPI RAW10: 4 pixels' upper 8 bits, then a byte of low bits.
            img[y * FRAME_W:(y + 1) * FRAME_W] = bytes(b for k, b in enumerate(line) if k % 5 != 4)
        with open(path, "wb") as f:
            f.write(b"P5\n%d %d\n255\n" % (FRAME_W, FRAME_H) + bytes(img))


def main():
    d = sys.argv[1]
    seconds = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 10
    nframes = int(sys.argv[sys.argv.index("--frames") + 1]) if "--frames" in sys.argv else 3
    outdir = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else "/root/camframes"

    prefix = sys.argv[sys.argv.index("--run") + 1] if "--run" in sys.argv else "run4"
    recs = list(read_log(os.path.join(d, prefix + "-cam.bin")))
    # The new HAL starts with its event subscriptions on cam-req-mgr.
    t0 = min(r["ts"] for r in recs if r["phase"] == "RAW_PRE" and r["minor"] == 0)
    t_last_start = max(r["ts"] for r in recs if r["phase"] == "PRE" and op_of(r) == OP_START_DEV)
    recs = [r for r in recs if t0 <= r["ts"] <= t_last_start + 10 ** 8]
    sb = [r for r in read_syncboss(os.path.join(d, prefix + "-syncboss-tx.trace"))
          if t0 - 10 ** 8 <= r["ts"] <= t_last_start + 2 * 10 ** 9]
    timeline = sorted(recs + sb, key=lambda r: r["ts"])
    # The recorder logs CONFIG_DEV before dumping its packet and command
    # buffers; they must be in memory before the call, so move the call after.
    fixed, i = [], 0
    while i < len(timeline):
        r = timeline[i]
        if r["phase"] == "PRE" and op_of(r) == OP_CONFIG_DEV:
            j = i + 1
            while j < len(timeline) and timeline[j]["phase"] in ("PACKET", "CMDBUF") \
                    and timeline[j]["src"] == r["src"]:
                j += 1
            fixed += timeline[i + 1:j] + [r]
            i = j
        else:
            fixed.append(r)
            i += 1
    timeline = fixed
    log("%d camera records, %d SyncBoss commands" % (len(recs), len(sb)))

    rp = Replayer()
    images = {}
    start = time.time()
    try:
        run(rp, timeline, images, start)
        log("set-up done: cameras %s" % [(hex(s), hex(dv)) for s, dv in rp.cams])
        rp.frame_loop(seconds, nframes, outdir, images)
    finally:
        rp.teardown()


def run(rp, timeline, images, start):
    for r in timeline:
        # Keep the recorded pacing (sensors/MCU need time between steps).
        due = start + (r["ts"] - timeline[0]["ts"]) / 1e9
        if due > time.time():
            time.sleep(due - time.time())
        ph = r["phase"]
        if ph == "SB":
            rp.do_syncboss(r)
        elif ph == "PRE":
            if op_of(r) == OP_SET_STREAM_MODE:
                # Remember which live buffer backs each image id, per camera.
                cam = len([c for c in rp.cams]) - 1
                pl = r["data"][24:]
                for k in range(struct.unpack_from("<I", pl, 8)[0]):
                    mh, = struct.unpack_from("<Q", pl, 16 + 48 * k)
                    iid, = struct.unpack_from("<Q", pl, 16 + 48 * k + 24)
                    images[(cam, iid)] = rp.map.get(mh & 0xffffffff)
                    ph, = struct.unpack_from("<Q", pl, 16 + 48 * k + 32)
                    if os.environ.get("QKX_DEBUG"):
                        lp = rp.map.get(ph & 0xffffffff)
                        log("cam %d img %d: mem %#x->%s pkt %#x->%s (len %s)" % (
                            cam, iid, mh, hex(images[(cam, iid)] or 0), ph, hex(lp or 0),
                            rp.bufs[lp][2] if lp in rp.bufs else None))
            rp.do_pre(r)
        elif ph == "POST":
            rp.do_post(r)
        elif ph == "RAW_PRE":
            rp.do_raw(r)
        elif ph == "PTR":
            rp.do_ptr(r)
        elif ph == "PTR_POST":
            rp.do_ptr_post(r)
        elif ph == "PACKET":
            rp.do_mem(r, translate=True)
        elif ph == "CMDBUF":
            rp.do_mem(r, translate=False)


if __name__ == "__main__":
    main()
