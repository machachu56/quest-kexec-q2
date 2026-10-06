// SPDX-License-Identifier: GPL-2.0
/*
 * Show a CPU-drawn ION buffer on the panel through msm_drm's PRIME import,
 * the same path gamescope uses for Turnip/KGSL buffers.
 *
 * Usage: ion2kms [seconds] [cached] [bars|grey] [split] [mode-index] [rotation]
 *   split: scan out through two half-width planes (one per layer mixer of
 *          the dual-DSI pipeline) with an atomic commit, instead of a single
 *          full-width legacy SetCrtc.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

struct ion_new_allocation_data {
	uint64_t len;
	uint32_t heap_id_mask;
	uint32_t flags;
	uint32_t fd;
	uint32_t unused;
};
#define ION_IOC_NEW_ALLOC _IOWR('I', 0, struct ion_new_allocation_data)
#define ION_SYSTEM_HEAP_MASK (1u << 25)
#define ION_FLAG_CACHED 1

static uint32_t prop_id(int fd, uint32_t obj, uint32_t type, const char *name)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
	uint32_t id = 0;

	for (uint32_t i = 0; props && i < props->count_props && !id; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);

		if (p && !strcmp(p->name, name))
			id = p->prop_id;
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	if (!id)
		fprintf(stderr, "no property %s on object %u\n", name, obj);
	return id;
}

static uint64_t g_rotation = 1;

static void add_plane(int fd, drmModeAtomicReq *req, uint32_t plane, uint32_t crtc,
		      uint32_t fb, uint32_t sx, uint32_t dx, uint32_t w, uint32_t h)
{
#define P(n, v) drmModeAtomicAddProperty(req, plane, prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, n), v)
	P("FB_ID", fb);
	P("CRTC_ID", crtc);
	P("SRC_X", (uint64_t)sx << 16);
	P("SRC_Y", 0);
	P("SRC_W", (uint64_t)w << 16);
	P("SRC_H", (uint64_t)h << 16);
	P("CRTC_X", dx);
	P("CRTC_Y", 0);
	P("CRTC_W", w);
	P("CRTC_H", h);
	if (g_rotation != 1)
		P("rotation", g_rotation);
#undef P
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 15;
	int cached = argc > 2 && atoi(argv[2]);
	int grey = argc > 3 && !strcmp(argv[3], "grey");
	int split = argc > 4 && !strcmp(argv[4], "split");
	int modei = argc > 5 ? atoi(argv[5]) : 0;
	int drm = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	drmModeRes *res;
	drmModeConnector *conn = NULL;
	drmModeModeInfo *mode;
	uint32_t crtc, handle, fb, w, h, pitch, i;
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	struct ion_new_allocation_data alloc = { 0 };
	int ion;
	uint32_t *px;

	if (argc > 6)
		g_rotation = strtoull(argv[6], NULL, 0);
	if (drm < 0 || drmSetMaster(drm))
		return perror("drm"), 1;
	drmSetClientCap(drm, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	drmSetClientCap(drm, DRM_CLIENT_CAP_ATOMIC, 1);
	res = drmModeGetResources(drm);
	for (i = 0; res && i < (uint32_t)res->count_connectors; i++) {
		conn = drmModeGetConnector(drm, res->connectors[i]);
		if (conn && conn->connector_type == DRM_MODE_CONNECTOR_DSI &&
		    conn->connection == DRM_MODE_CONNECTED && conn->count_modes > modei)
			break;
		drmModeFreeConnector(conn);
		conn = NULL;
	}
	if (!conn)
		return fprintf(stderr, "no connected DSI connector\n"), 1;
	crtc = res->crtcs[0];
	mode = &conn->modes[modei];
	w = mode->hdisplay;
	h = mode->vdisplay;
	pitch = w * 4;

	ion = open("/dev/ion", O_RDONLY | O_CLOEXEC);
	alloc.len = (uint64_t)pitch * h;
	alloc.heap_id_mask = ION_SYSTEM_HEAP_MASK;
	alloc.flags = cached ? ION_FLAG_CACHED : 0;
	if (ion < 0 || ioctl(ion, ION_IOC_NEW_ALLOC, &alloc))
		return perror("ion alloc"), 1;
	px = mmap(NULL, alloc.len, PROT_READ | PROT_WRITE, MAP_SHARED, alloc.fd, 0);
	if (px == MAP_FAILED)
		return perror("mmap"), 1;
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			static const uint32_t bars[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
							  0xff00ff, 0xff0000, 0x0000ff, 0x000000 };
			uint32_t c = grey ? 0x808080 : bars[x * 8 / w];

			if (!grey && (x < 8 || y < 8 || x >= w - 8 || y >= h - 8 || x == y * w / h))
				c = 0xffffff;
			px[y * w + x] = c;
		}
	munmap(px, alloc.len);

	if (drmPrimeFDToHandle(drm, alloc.fd, &handle))
		return perror("drmPrimeFDToHandle"), 1;
	handles[0] = handle;
	pitches[0] = pitch;
	if (drmModeAddFB2(drm, w, h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &fb, 0))
		return perror("drmModeAddFB2"), 1;

	if (!split) {
		if (drmModeSetCrtc(drm, crtc, fb, 0, 0, &conn->connector_id, 1, mode))
			return perror("drmModeSetCrtc"), 1;
	} else {
		drmModePlaneRes *pr = drmModeGetPlaneResources(drm);
		uint32_t planes[2] = { 0 }, n = 0, blob;
		drmModeAtomicReq *req = drmModeAtomicAlloc();

		/* The first two planes usable on this CRTC (splash used 58 and 80). */
		for (i = 0; pr && i < pr->count_planes && n < 2; i++) {
			drmModePlane *p = drmModeGetPlane(drm, pr->planes[i]);

			if (p && (p->possible_crtcs & 1))
				planes[n++] = p->plane_id;
			drmModeFreePlane(p);
		}
		if (n < 2)
			return fprintf(stderr, "need two planes\n"), 1;
		if (drmModeCreatePropertyBlob(drm, mode, sizeof(*mode), &blob))
			return perror("mode blob"), 1;
		drmModeAtomicAddProperty(req, crtc, prop_id(drm, crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID"), blob);
		drmModeAtomicAddProperty(req, crtc, prop_id(drm, crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE"), 1);
		drmModeAtomicAddProperty(req, conn->connector_id,
			prop_id(drm, conn->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"), crtc);
		add_plane(drm, req, planes[0], crtc, fb, 0, 0, w / 2, h);
		add_plane(drm, req, planes[1], crtc, fb, w / 2, w / 2, w / 2, h);
		if (drmModeAtomicCommit(drm, req, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL)) {
			perror("atomic test commit");
			return 2;
		}
		if (drmModeAtomicCommit(drm, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL))
			return perror("atomic commit"), 1;
		printf("split across planes %u and %u\n", planes[0], planes[1]);
	}
	printf("showing %ux%u %s ION (%s) buffer, mode %d (%u Hz), fb %u for %d s\n",
	       w, h, grey ? "grey" : "bars", cached ? "cached" : "uncached", modei,
	       mode->vrefresh, fb, secs);
	sleep(secs);
	return 0;
}
