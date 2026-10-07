// Copyright 2026, quest-kexec contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Quest 2 tracking cameras: replay of the recorded camera start-up.
 *
 * The four OV7251 cameras are powered and exposed by SyncBoss; the SoC side is
 * Qualcomm's Spectra driver (CSIPHY -> IFE raw dump) with Meta's stream-mode
 * frame API. There is no open userspace for it, so the start-up done by
 * Meta's camera HAL is recorded on Android (quest-kexec module/qkx_camtrace)
 * and compiled into a script (tools/camtrace/mkscript.py) that is replayed
 * here. Kernel-generated values (sessions, devices, links, memory and IOMMU
 * handles) are learnt from the live results and substituted into later
 * arguments and packets; Android's gralloc image buffers become driver
 * allocations. tools/camtrace/replay.py is the reference implementation.
 *
 * @ingroup drv_quest2
 */

#include "q2_cameras.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_frame.h"
#include "util/u_logging.h"
#include "util/u_misc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CAM_LOG(...) U_LOG_I("q2 cameras: " __VA_ARGS__)
#define CAM_ERR(...) U_LOG_E("q2 cameras: " __VA_ARGS__)

#define VIDIOC_CAM_CONTROL 0xC01856C0u
#define OP_QUERY_CAP 0x101
#define OP_ACQUIRE_DEV 0x102
#define OP_START_DEV 0x103
#define OP_STOP_DEV 0x104
#define OP_RELEASE_DEV 0x106
#define OP_SET_STREAM_MODE 0x109
#define OP_STREAM_MODE_CMD 0x10a
#define OP_ALLOC 0x114
#define OP_MAP 0x115
#define OP_ACQUIRE_HW 0x151
#define ISP_MINOR 129

#define IMAGE_LEN (512 * 1024) // 800 x 481 RAW10
#define RAW_STRIDE 800
#define MAX_MAP 1024
#define MAX_BUFS 256
#define MAX_UNDO 64
#define MAX_IMAGES 30

struct cam_control
{
	uint32_t op_code, size, handle_type, reserved;
	uint64_t handle;
};

struct buf
{
	uint32_t handle;
	int fd;
	uint8_t *map;
	size_t len;
};

struct cam
{
	int32_t session, dev;
	uint32_t n_images;
	uint64_t image_id[MAX_IMAGES];
	uint32_t image_handle[MAX_IMAGES];
	struct os_thread thread;
	struct q2_cameras *owner;
	int index;
	struct xrt_frame_sink *sink;
	uint64_t frames;
};

struct undo
{
	uint16_t minor;
	uint32_t op;
	int32_t session, dev;
};

struct q2_cameras
{
	int fds[256];
	uint32_t map_from[MAX_MAP], map_to[MAX_MAP];
	bool map_small[MAX_MAP];
	int n_map;
	struct buf bufs[MAX_BUFS];
	int n_bufs;
	struct undo undo[MAX_UNDO];
	int n_undo;
	struct cam cams[Q2_CAMERA_COUNT];
	int n_cams;
	volatile bool running;
};


/*
 *
 * Handle translation.
 *
 */

static bool
map_get(struct q2_cameras *c, uint32_t v, uint32_t *out)
{
	for (int i = c->n_map - 1; i >= 0; i--) {
		if (c->map_from[i] == v) {
			*out = c->map_to[i];
			return true;
		}
	}
	return false;
}

static void
map_put(struct q2_cameras *c, uint32_t from, uint32_t to)
{
	if (c->n_map < MAX_MAP) {
		c->map_from[c->n_map] = from;
		c->map_to[c->n_map] = to;
		c->n_map++;
	}
}

static void
translate(struct q2_cameras *c, uint8_t *b, uint32_t len)
{
	for (uint32_t i = 0; i + 4 <= len; i += 4) {
		uint32_t v, t;
		memcpy(&v, b + i, 4);
		if (v >= 0x100 && map_get(c, v, &t)) {
			memcpy(b + i, &t, 4);
		}
	}
}

/*!
 * Words that differ between the recorded arguments and the recorded result
 * are outputs: map them to the live result. Session/device/memory handles
 * have a counter in their upper half; smaller outputs (lengths, counts) are
 * only taken when @p small_ok (IOMMU handles from capability queries).
 */
static void
learn(struct q2_cameras *c, const uint8_t *pre, const uint8_t *post, const uint8_t *live, uint32_t len, bool small_ok)
{
	for (uint32_t i = 0; i + 4 <= len; i += 4) {
		uint32_t a, b, l;
		memcpy(&a, pre + i, 4);
		memcpy(&b, post + i, 4);
		memcpy(&l, live + i, 4);
		if (a != b && (b >= 0x10000 || (small_ok && b >= 0x100))) {
			map_put(c, b, l);
		}
	}
}


/*
 *
 * Devices and memory.
 *
 */

static int
node_fd(struct q2_cameras *c, uint16_t minor)
{
	if (minor >= 256) {
		return -1;
	}
	if (c->fds[minor] < 0) {
		char path[64];
		if (minor < 64) {
			snprintf(path, sizeof(path), "/dev/video%u", minor);
		} else {
			snprintf(path, sizeof(path), "/dev/v4l-subdev%u", minor - 128);
		}
		c->fds[minor] = open(path, O_RDWR | O_NONBLOCK);
		if (c->fds[minor] < 0) {
			CAM_ERR("open %s: %s", path, strerror(errno));
		}
	}
	return c->fds[minor];
}

static int
cam_control(struct q2_cameras *c, uint16_t minor, uint32_t op, uint32_t size, uint32_t htype, uint64_t handle, void *arg)
{
	struct cam_control ctl = {op, size, htype, 0, htype == 2 ? handle : (uint64_t)(uintptr_t)arg};
	int fd = node_fd(c, minor);
	if (fd < 0) {
		return -ENODEV;
	}
	return ioctl(fd, VIDIOC_CAM_CONTROL, &ctl) < 0 ? -errno : 0;
}

static struct buf *
buf_find(struct q2_cameras *c, uint32_t handle)
{
	for (int i = 0; i < c->n_bufs; i++) {
		if (c->bufs[i].handle == handle) {
			return &c->bufs[i];
		}
	}
	return NULL;
}

static void
buf_add(struct q2_cameras *c, const uint8_t *alloc_out)
{
	uint64_t len;
	uint32_t handle;
	int32_t fd;
	memcpy(&len, alloc_out, 8);
	memcpy(&handle, alloc_out + 88, 4);
	memcpy(&fd, alloc_out + 92, 4);
	if (c->n_bufs >= MAX_BUFS) {
		return;
	}
	uint8_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	c->bufs[c->n_bufs++] = (struct buf){handle, fd, m == MAP_FAILED ? NULL : m, len};
}


/*
 *
 * Script.
 *
 */

struct reader
{
	const uint8_t *p, *end;
	bool bad;
};

static uint32_t
rd_u32(struct reader *r)
{
	uint32_t v = 0;
	if (r->p + 4 > r->end) {
		r->bad = true;
		return 0;
	}
	memcpy(&v, r->p, 4);
	r->p += 4;
	return v;
}

static uint64_t
rd_u64(struct reader *r)
{
	uint64_t v = 0;
	if (r->p + 8 > r->end) {
		r->bad = true;
		return 0;
	}
	memcpy(&v, r->p, 8);
	r->p += 8;
	return v;
}

static uint16_t
rd_u16(struct reader *r)
{
	uint16_t v = 0;
	if (r->p + 2 > r->end) {
		r->bad = true;
		return 0;
	}
	memcpy(&v, r->p, 2);
	r->p += 2;
	return v;
}

static const uint8_t *
rd_blob(struct reader *r, uint32_t *len)
{
	*len = rd_u32(r);
	if (r->bad || r->p + *len > r->end) {
		r->bad = true;
		return NULL;
	}
	const uint8_t *b = r->p;
	r->p += *len;
	return b;
}

static void
do_camctl(struct q2_cameras *c, struct reader *r)
{
	uint16_t minor = rd_u16(r);
	uint32_t op = rd_u32(r), size = rd_u32(r), htype = rd_u32(r);
	uint64_t handle = rd_u64(r);
	uint32_t plen;
	const uint8_t *payload = rd_blob(r, &plen);

	struct
	{
		uint32_t offset, len;
		const uint8_t *rec;
		uint8_t *live;
	} ptrs[8];
	uint32_t nptr = rd_u32(r);
	for (uint32_t i = 0; i < nptr && !r->bad; i++) {
		uint32_t off = rd_u32(r), len;
		const uint8_t *d = rd_blob(r, &len);
		if (i < 8) {
			ptrs[i].offset = off;
			ptrs[i].len = len;
			ptrs[i].rec = d;
		}
	}
	uint32_t post_len;
	const uint8_t *post = rd_blob(r, &post_len);
	uint32_t npost = rd_u32(r);
	struct
	{
		uint32_t offset, len;
		const uint8_t *data;
	} posts[8];
	for (uint32_t i = 0; i < npost && !r->bad; i++) {
		uint32_t off = rd_u32(r), len;
		const uint8_t *d = rd_blob(r, &len);
		if (i < 8) {
			posts[i].offset = off;
			posts[i].len = len;
			posts[i].data = d;
		}
	}
	if (r->bad) {
		return;
	}
	nptr = nptr > 8 ? 8 : nptr;
	npost = npost > 8 ? 8 : npost;

	uint8_t *live = calloc(1, plen > size ? plen : size + 8);
	memcpy(live, payload, plen);

	if (minor == 0 && op == OP_MAP) {
		/* Android maps a gralloc buffer: allocate an image buffer instead,
		 * through the same IOMMU handles. */
		uint8_t a[104] = {0};
		uint64_t len = IMAGE_LEN, align = 4096;
		uint32_t nh, flags = 0x1 | 0x10 | 0x80;
		memcpy(&nh, payload + 64, 4);
		memcpy(a, &len, 8);
		memcpy(a + 8, &align, 8);
		for (uint32_t i = 0; i < nh && i < 16; i++) {
			uint32_t h, t;
			memcpy(&h, payload + 4 * i, 4);
			if (map_get(c, h, &t)) {
				h = t;
			}
			memcpy(a + 16 + 4 * i, &h, 4);
		}
		memcpy(a + 80, &nh, 4);
		memcpy(a + 84, &flags, 4);
		if (cam_control(c, 0, OP_ALLOC, sizeof(a), 0, 0, a) == 0) {
			buf_add(c, a);
			memcpy(live + 80, a + 88, 4);
			if (post_len >= 84) {
				learn(c, payload, post, live, 84, false);
			}
		} else {
			CAM_ERR("image buffer allocation failed");
		}
		free(live);
		return;
	}

	translate(c, live, plen);
	for (uint32_t i = 0; i < nptr; i++) {
		ptrs[i].live = malloc(ptrs[i].len);
		memcpy(ptrs[i].live, ptrs[i].rec, ptrs[i].len);
		translate(c, ptrs[i].live, ptrs[i].len);
		uint64_t addr = (uint64_t)(uintptr_t)ptrs[i].live;
		if (ptrs[i].offset + 8 <= plen) {
			memcpy(live + ptrs[i].offset, &addr, 8);
		}
	}
	uint64_t h = handle;
	if (htype == 2) {
		uint32_t t;
		if (map_get(c, (uint32_t)h, &t)) {
			h = t;
		}
	}

	int ret = cam_control(c, minor, op, size, htype, h, live);
	if (ret != 0) {
		CAM_ERR("op %#x on node %u failed: %d", op, minor, ret);
	}

	if (ret == 0) {
		if (post_len) {
			learn(c, payload, post, live, plen < post_len ? plen : post_len, op == OP_QUERY_CAP);
		}
		for (uint32_t i = 0; i < npost; i++) {
			for (uint32_t j = 0; j < nptr; j++) {
				if (posts[i].offset == ptrs[j].offset) {
					uint32_t n = posts[i].len < ptrs[j].len ? posts[i].len : ptrs[j].len;
					learn(c, ptrs[j].rec, posts[i].data, ptrs[j].live, n, true);
				}
			}
		}
		if (minor == 0 && op == OP_ALLOC) {
			buf_add(c, live);
		}
		if ((op == OP_ACQUIRE_DEV || op == OP_START_DEV) && c->n_undo < MAX_UNDO) {
			int32_t s, d;
			memcpy(&s, live, 4);
			memcpy(&d, live + 4, 4);
			c->undo[c->n_undo++] = (struct undo){minor, op, s, d};
		}
		if (minor == ISP_MINOR && op == OP_ACQUIRE_DEV && c->n_cams < Q2_CAMERA_COUNT) {
			struct cam *cam = &c->cams[c->n_cams++];
			memcpy(&cam->session, live, 4);
			memcpy(&cam->dev, live + 4, 4);
		}
		if (minor == ISP_MINOR && op == OP_SET_STREAM_MODE && c->n_cams > 0) {
			struct cam *cam = &c->cams[c->n_cams - 1];
			uint32_t n;
			memcpy(&n, live + 8, 4);
			cam->n_images = n > MAX_IMAGES ? MAX_IMAGES : n;
			for (uint32_t i = 0; i < cam->n_images; i++) {
				uint64_t mh;
				memcpy(&mh, live + 16 + 48 * i, 8);
				memcpy(&cam->image_id[i], live + 16 + 48 * i + 24, 8);
				cam->image_handle[i] = (uint32_t)mh;
			}
		}
	}
	for (uint32_t i = 0; i < nptr; i++) {
		free(ptrs[i].live);
	}
	free(live);
}

static void
do_mem(struct q2_cameras *c, struct reader *r)
{
	uint32_t handle = rd_u32(r);
	uint64_t offset = rd_u64(r);
	uint8_t tr = r->p < r->end ? *r->p++ : 0;
	uint32_t len;
	const uint8_t *data = rd_blob(r, &len);
	uint32_t live_handle;
	if (r->bad || !map_get(c, handle, &live_handle)) {
		CAM_ERR("no live buffer for recorded handle %#x", handle);
		return;
	}
	struct buf *b = buf_find(c, live_handle);
	if (b == NULL || b->map == NULL || offset + len > b->len) {
		return;
	}
	memcpy(b->map + offset, data, len);
	if (tr) {
		translate(c, b->map + offset, len);
	}
}

static void
do_raw(struct q2_cameras *c, struct reader *r)
{
	uint16_t minor = rd_u16(r);
	uint32_t cmd = rd_u32(r), len;
	const uint8_t *data = rd_blob(r, &len);
	if (r->bad) {
		return;
	}
	uint8_t tmp[256] = {0};
	memcpy(tmp, data, len < sizeof(tmp) ? len : sizeof(tmp));
	int fd = node_fd(c, minor);
	if (fd >= 0 && ioctl(fd, cmd, tmp) < 0) {
		CAM_ERR("ioctl %#x on node %u: %s", cmd, minor, strerror(errno));
	}
}

static bool
run_script(struct q2_cameras *c, const char *path, q2_syncboss_send_fn sb_send, void *sb_ctx)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		CAM_LOG("no camera script at %s: cameras disabled", path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *data = malloc(n);
	bool ok = fread(data, 1, n, f) == (size_t)n;
	fclose(f);

	struct reader r = {data, data + n, false};
	uint32_t version = 0, count = 0;
	if (!ok || n < 12 || memcmp(data, "Q2CS", 4) != 0) {
		CAM_ERR("%s: not a camera script", path);
		free(data);
		return false;
	}
	r.p += 4;
	version = rd_u32(&r);
	count = rd_u32(&r);
	(void)version;

	for (uint32_t i = 0; i < count && !r.bad; i++) {
		uint8_t kind = *r.p++;
		uint32_t delay_us = rd_u32(&r);
		if (delay_us > 0 && delay_us < 2000000) {
			os_nanosleep((int64_t)delay_us * 1000);
		}
		switch (kind) {
		case 1: do_camctl(c, &r); break;
		case 2: do_raw(c, &r); break;
		case 3: do_mem(c, &r); break;
		case 4: {
			uint32_t len;
			const uint8_t *pkt = rd_blob(&r, &len);
			if (pkt != NULL && pkt[0] != 90 && pkt[0] != 203 && pkt[0] != 204) {
				sb_send(sb_ctx, pkt, len);
			}
			break;
		}
		default: r.bad = true; break;
		}
	}
	free(data);
	if (r.bad) {
		CAM_ERR("%s: truncated or corrupt script", path);
		return false;
	}
	return c->n_cams > 0;
}


/*
 *
 * Frames.
 *
 */

static uint8_t *
image_map(struct q2_cameras *c, struct cam *cam, uint64_t image_id)
{
	for (uint32_t i = 0; i < cam->n_images; i++) {
		if (cam->image_id[i] == image_id) {
			struct buf *b = buf_find(c, cam->image_handle[i]);
			return b ? b->map : NULL;
		}
	}
	return NULL;
}

static void *
frame_thread(void *ptr)
{
	struct cam *cam = ptr;
	struct q2_cameras *c = cam->owner;
	uint8_t cmd[1256];
	uint64_t ret_ids[MAX_IMAGES];
	uint32_t n_ret = 0;

	while (c->running) {
		memset(cmd, 0, sizeof(cmd));
		uint32_t mode = 3; // return images and get new ones
		memcpy(cmd, &cam->session, 4);
		memcpy(cmd + 4, &cam->dev, 4);
		memcpy(cmd + 8, &mode, 4);
		memcpy(cmd + 16, &n_ret, 4);
		memcpy(cmd + 24, ret_ids, 8 * n_ret);
		uint32_t timeout = 100;
		memcpy(cmd + 264, &timeout, 4);
		n_ret = 0;

		if (cam_control(c, ISP_MINOR, OP_STREAM_MODE_CMD, sizeof(cmd), 1, 0, cmd) != 0) {
			continue;
		}
		uint32_t n;
		memcpy(&n, cmd + 268, 4);
		for (uint32_t k = 0; k < n && k < MAX_IMAGES; k++) {
			uint64_t id, ts, sof;
			memcpy(&id, cmd + 272 + 32 * k, 8);
			memcpy(&ts, cmd + 272 + 32 * k + 8, 8);
			memcpy(&sof, cmd + 272 + 32 * k + 16, 8);
			ret_ids[n_ret++] = id;
			cam->frames++;

			uint8_t *raw = image_map(c, cam, id);
			if (cam->sink == NULL || raw == NULL) {
				continue;
			}
			struct xrt_frame *xf = NULL;
			u_frame_create_one_off(XRT_FORMAT_L8, Q2_CAMERA_W, Q2_CAMERA_H, &xf);
			/* MIPI RAW10: 4 pixels' upper 8 bits, then a byte of low bits. */
			for (int y = 0; y < Q2_CAMERA_H; y++) {
				const uint8_t *s = raw + y * RAW_STRIDE;
				uint8_t *d = xf->data + y * xf->stride;
				for (int x = 0; x < Q2_CAMERA_W; x += 4, s += 5) {
					d[x] = s[0];
					d[x + 1] = s[1];
					d[x + 2] = s[2];
					d[x + 3] = s[3];
				}
			}
			xf->timestamp = (int64_t)sof;
			xf->source_timestamp = (int64_t)ts;
			xf->source_sequence = cam->frames;
			xf->source_id = cam->index;
			xrt_sink_push_frame(cam->sink, xf);
			xrt_frame_reference(&xf, NULL);
		}
	}
	return NULL;
}


/*
 *
 * Interface.
 *
 */

struct q2_cameras *
q2_cameras_start(const char *script_path,
                 q2_syncboss_send_fn sb_send,
                 void *sb_ctx,
                 struct xrt_frame_sink *sinks[Q2_CAMERA_COUNT])
{
	struct q2_cameras *c = U_TYPED_CALLOC(struct q2_cameras);
	for (int i = 0; i < 256; i++) {
		c->fds[i] = -1;
	}

	if (!run_script(c, script_path, sb_send, sb_ctx)) {
		q2_cameras_stop(&c);
		return NULL;
	}

	c->running = true;
	for (int i = 0; i < c->n_cams; i++) {
		struct cam *cam = &c->cams[i];
		cam->owner = c;
		cam->index = i;
		cam->sink = sinks ? sinks[i] : NULL;
		os_thread_init(&cam->thread);
		os_thread_start(&cam->thread, frame_thread, cam);
	}
	CAM_LOG("%d cameras streaming", c->n_cams);
	return c;
}

void
q2_cameras_stop(struct q2_cameras **cams_ptr)
{
	struct q2_cameras *c = *cams_ptr;
	if (c == NULL) {
		return;
	}
	if (c->running) {
		c->running = false;
		for (int i = 0; i < c->n_cams; i++) {
			os_thread_join(&c->cams[i].thread);
			os_thread_destroy(&c->cams[i].thread);
		}
	}
	/* Undo in reverse: stop started devices, release acquired ones. */
	for (int i = c->n_undo - 1; i >= 0; i--) {
		struct undo *u = &c->undo[i];
		int32_t sd[2] = {u->session, u->dev};
		cam_control(c, u->minor, u->op == OP_START_DEV ? OP_STOP_DEV : OP_RELEASE_DEV, sizeof(sd), 1, 0, sd);
	}
	for (int i = 0; i < c->n_bufs; i++) {
		if (c->bufs[i].map != NULL) {
			munmap(c->bufs[i].map, c->bufs[i].len);
		}
		if (c->bufs[i].fd >= 0) {
			close(c->bufs[i].fd);
		}
	}
	for (int i = 0; i < 256; i++) {
		if (c->fds[i] >= 0) {
			close(c->fds[i]);
		}
	}
	free(c);
	*cams_ptr = NULL;
}
