// Copyright 2026, quest-kexec contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Meta Quest 2 headset and Touch controllers through SyncBoss.
 *
 * SyncBoss is the headset's nRF52 MCU; Meta's kernel driver exposes it as
 * /dev/syncboss0 (commands) and /dev/syncboss_stream0 (packets). Protocol
 * notes are in quest-kexec docs/QUEST2_PORT.md:
 *
 * - command 110 [imu id] enables a headset IMU: packet type 80, ~1 kHz,
 *   u64 timestamp (us), accel xyz (f32, g), gyro xyz (f32, deg/s), temperature.
 * - command 133 [0] starts the controller radio: packet type 143 per
 *   controller, 8-byte id, 8-byte descriptor (byte 10: 0 left, 1 right),
 *   8 bytes link info, then [key, flags, data] records (lengths below).
 *
 * This first version gives 3DoF (IMU fusion) with fixed positions; 6DoF needs
 * the tracking cameras (see tools/camtrace in quest-kexec).
 *
 * @ingroup drv_quest2
 */

#include "xrt/xrt_defines.h"
#include "xrt/xrt_device.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include "math/m_api.h"
#include "math/m_imu_3dof.h"
#include "math/m_mathinclude.h"
#include "math/m_relation_history.h"
#include "math/m_space.h"

#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_var.h"
#include "util/u_visibility_mask.h"

#include "q2_interface.h"
#include "q2_cameras.h"

#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

DEBUG_GET_ONCE_LOG_OPTION(q2_log, "QUEST2_LOG", U_LOGGING_INFO)
DEBUG_GET_ONCE_OPTION(q2_camera_script, "QUEST2_CAMERA_SCRIPT", "/usr/local/share/qkx/q2-cameras.bin")
DEBUG_GET_ONCE_OPTION(q2_dump_frames, "QUEST2_DUMP_FRAMES", NULL)

#define Q2_DEBUG(...) U_LOG_IFL_D(q2_log_level, __VA_ARGS__)
#define Q2_INFO(...) U_LOG_IFL_I(q2_log_level, __VA_ARGS__)
#define Q2_ERROR(...) U_LOG_IFL_E(q2_log_level, __VA_ARGS__)

static enum u_logging_level q2_log_level;

#define SB_DEV "/dev/syncboss0"
#define SB_STREAM "/dev/syncboss_stream0"
#define SB_SYSFS "/sys/devices/virtual/misc/syncboss0/spi/control/transaction_length"
#define SB_SEQ_ALLOC _IOR(10, 2, uint8_t)
#define SB_SEQ_RELEASE _IOW(10, 3, uint8_t)

#define SB_PKT_HMD_IMU 80
#define SB_PKT_CONTROLLER 143

#define G 9.80665f
#define DEG2RAD ((float)M_PI / 180.0f)


/*
 *
 * Structs.
 *
 */

enum q2_touch_input
{
	/* Left */
	Q2_X_CLICK = 0,
	Q2_X_TOUCH,
	Q2_Y_CLICK,
	Q2_Y_TOUCH,
	Q2_MENU_CLICK,
	/* Right */
	Q2_A_CLICK = 0,
	Q2_A_TOUCH,
	Q2_B_CLICK,
	Q2_B_TOUCH,
	Q2_SYSTEM_CLICK,
	/* Common */
	Q2_SQUEEZE_VALUE,
	Q2_TRIGGER_TOUCH,
	Q2_TRIGGER_VALUE,
	Q2_THUMBSTICK_CLICK,
	Q2_THUMBSTICK_TOUCH,
	Q2_THUMBSTICK,
	Q2_THUMBREST_TOUCH,
	Q2_GRIP_POSE,
	Q2_AIM_POSE,
	Q2_INPUT_COUNT
};

struct q2_system;

struct q2_controller
{
	struct xrt_device base;
	struct q2_system *sys;
	bool left;

	//! Protected by sys->lock.
	struct m_imu_3dof fusion;
	uint8_t buttons;
	float trigger, grip;
	struct xrt_vec2 stick;
	int64_t last_ns;
	uint32_t last_dev_us;
	int64_t dev_offset_ns;
	bool have_offset;
};

struct q2_hmd
{
	struct xrt_device base;
	struct q2_system *sys;
	struct m_relation_history *relation_hist;
	//! Protected by sys->lock.
	struct m_imu_3dof fusion;
	int64_t dev_offset_ns;
	bool have_offset;
	struct xrt_vec3 gyro_bias;
	uint32_t still_count;
};

/*!
 * Per-camera frame statistics; frames alternate between normal exposures
 * (head tracking) and short ones where only the controllers' LEDs show.
 */
struct q2_frame_stats
{
	struct xrt_frame_sink base;
	int cam;
	uint64_t frames, led_frames;
};

struct q2_system
{
	int refs;
	uint64_t n_imu, n_ctrl;
	struct q2_cameras *cameras;
	struct q2_frame_stats stats[Q2_CAMERA_COUNT];
	struct os_mutex lock;
	struct os_thread_helper thread;
	int dev_fd, stream_fd;
	struct q2_hmd *hmd;
	struct q2_controller *ctrl[2];
};


/*
 *
 * SyncBoss.
 *
 */

static int
sb_send(struct q2_system *sys, uint8_t type, const uint8_t *data, uint8_t len)
{
	uint8_t seq = 0;
	uint8_t buf[3 + 255];

	if (ioctl(sys->dev_fd, SB_SEQ_ALLOC, &seq) < 0) {
		seq = 0;
	}
	buf[0] = type;
	buf[1] = seq;
	buf[2] = len;
	memcpy(buf + 3, data, len);
	ssize_t r = write(sys->dev_fd, buf, 3 + len);
	if (seq != 0) {
		ioctl(sys->dev_fd, SB_SEQ_RELEASE, &seq);
	}
	return r == 3 + len ? 0 : -1;
}

/*!
 * Map a device timestamp to the host clock: the smallest (arrival - device)
 * offset seen is the one with the least transport latency.
 */
static int64_t
map_time(int64_t dev_ns, int64_t now_ns, int64_t *offset, bool *have)
{
	int64_t off = now_ns - dev_ns;
	if (!*have || off < *offset) {
		*offset = off;
		*have = true;
	} else {
		// Let the offset drift slowly upwards (clock rate differences).
		*offset += 1000;
	}
	return dev_ns + *offset;
}

static void
handle_hmd_imu(struct q2_system *sys, const uint8_t *d, uint8_t len, int64_t now_ns)
{
	struct q2_hmd *hmd = sys->hmd;
	if (hmd == NULL || len < 32) {
		return;
	}

	uint64_t ts_us;
	float a[3], g[3];
	memcpy(&ts_us, d, 8);
	memcpy(a, d + 8, 12);
	memcpy(g, d + 20, 12);

	/* IMU -> headset frame (x right, y up, z back), from the factory
	 * rectification matrix: swap x/y, flip z. */
	struct xrt_vec3 accel = {a[1] * G, a[0] * G, -a[2] * G};
	struct xrt_vec3 gyro = {g[1] * DEG2RAD, g[0] * DEG2RAD, -g[2] * DEG2RAD};

	/* Simple gyro bias tracking while still. */
	float glen = sqrtf(gyro.x * gyro.x + gyro.y * gyro.y + gyro.z * gyro.z);
	float alen = sqrtf(accel.x * accel.x + accel.y * accel.y + accel.z * accel.z);
	if (glen < 0.05f && fabsf(alen - G) < 0.3f) {
		const float k = 0.001f;
		hmd->gyro_bias.x += (gyro.x - hmd->gyro_bias.x) * k;
		hmd->gyro_bias.y += (gyro.y - hmd->gyro_bias.y) * k;
		hmd->gyro_bias.z += (gyro.z - hmd->gyro_bias.z) * k;
	}
	gyro.x -= hmd->gyro_bias.x;
	gyro.y -= hmd->gyro_bias.y;
	gyro.z -= hmd->gyro_bias.z;

	int64_t ts = map_time((int64_t)ts_us * 1000, now_ns, &hmd->dev_offset_ns, &hmd->have_offset);
	m_imu_3dof_update(&hmd->fusion, ts, &accel, &gyro);

	struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
	rel.pose.orientation = hmd->fusion.rot;
	rel.pose.position = (struct xrt_vec3){0, 1.6f, 0};
	math_quat_rotate_derivative(&rel.pose.orientation, &gyro, &rel.angular_velocity);
	rel.relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	    XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
	m_relation_history_push(hmd->relation_hist, &rel, ts);
}

static void
handle_ctrl_motion(struct q2_controller *c, uint32_t dev_us, const int16_t *v, int64_t now_ns)
{
	/* ICM-42686 at +-32 g / +-4000 dps: 1024 LSB/g, 8.2 LSB/(deg/s).
	 * IMU -> controller (LED model) frame, from the TrackedObject
	 * calibration: the two controllers mount their IMUs mirrored. */
	const float as = G / 1024.0f, gs = DEG2RAD / 8.2f;
	struct xrt_vec3 accel, gyro;
	if (c->left) {
		accel = (struct xrt_vec3){v[0] * as, -v[2] * as, v[1] * as};
		gyro = (struct xrt_vec3){v[3] * gs, -v[5] * gs, v[4] * gs};
	} else {
		accel = (struct xrt_vec3){-v[0] * as, -v[2] * as, -v[1] * as};
		gyro = (struct xrt_vec3){-v[3] * gs, -v[5] * gs, -v[4] * gs};
	}

	int64_t ts = map_time((int64_t)dev_us * 1000, now_ns, &c->dev_offset_ns, &c->have_offset);
	if (ts <= c->last_ns) {
		ts = c->last_ns + 1;
	}
	c->last_ns = ts;
	m_imu_3dof_update(&c->fusion, ts, &accel, &gyro);
}

static void
handle_controller(struct q2_system *sys, const uint8_t *p, uint8_t len, int64_t now_ns)
{
	/* Record lengths including key byte (see qkx-controllers). */
	static const uint8_t reclen[256] = {
	    [0x07] = 15, [0x08] = 15, [0x20] = 3, [0x22] = 3, [0x24] = 3, [0x41] = 20, [0x45] = 4,
	    [0x62] = 5,  [0x63] = 5,  [0x81] = 14, [0x82] = 6, [0xc1] = 8, [0xff] = 2,
	};
	if (len < 24 || p[10] > 1) {
		return;
	}
	struct q2_controller *c = sys->ctrl[p[10] == 0 ? 0 : 1];
	if (c == NULL) {
		return;
	}

	const uint8_t *r = p + 24, *end = p + len;
	while (r < end && reclen[*r] != 0 && r + reclen[*r] <= end) {
		const uint8_t *v = r + 1;
		switch (*r) {
		case 0x24: c->buttons = v[1]; break;
		case 0x63: {
			uint32_t raw = v[1] | (v[2] << 8) | ((uint32_t)v[3] << 16);
			c->trigger = (0xfff - (raw & 0xfff)) / 4095.0f;
			c->grip = (0xfff - (raw >> 12)) / 4095.0f;
			break;
		}
		case 0x82: {
			int16_t x, y;
			memcpy(&x, v + 1, 2);
			memcpy(&y, v + 3, 2);
			c->stick.x = x / 32767.0f;
			c->stick.y = y / 32767.0f;
			break;
		}
		case 0x41: {
			uint32_t dev_us;
			int16_t m[6];
			memcpy(&dev_us, v + 1, 4);
			memcpy(m, v + 7, 12);
			c->last_dev_us = dev_us;
			handle_ctrl_motion(c, dev_us, m, now_ns);
			break;
		}
		case 0x81: {
			/* Motion without timestamp: the next sample (~2 ms). */
			int16_t m[6];
			memcpy(m, v + 1, 12);
			c->last_dev_us += 2000;
			handle_ctrl_motion(c, c->last_dev_us, m, now_ns);
			break;
		}
		default: break;
		}
		r += reclen[*r];
	}
}

static void *
q2_run(void *ptr)
{
	struct q2_system *sys = ptr;
	uint8_t buf[65536];
	int64_t next_report = os_monotonic_get_ns() + U_TIME_1S_IN_NS;

	os_thread_helper_lock(&sys->thread);
	while (os_thread_helper_is_running_locked(&sys->thread)) {
		os_thread_helper_unlock(&sys->thread);

		struct pollfd pfd = {.fd = sys->stream_fd, .events = POLLIN};
		if (poll(&pfd, 1, 100) <= 0) {
			os_thread_helper_lock(&sys->thread);
			continue;
		}
		/* A read returns few packets; drain everything that is queued. */
		ssize_t n = 0, r;
		while (n < (ssize_t)sizeof(buf) - 512 && (r = read(sys->stream_fd, buf + n, sizeof(buf) - n)) > 0) {
			n += r;
		}
		int64_t now = os_monotonic_get_ns();

		os_mutex_lock(&sys->lock);
		for (ssize_t i = 0; n > 0 && i + 3 < n;) {
			uint8_t hl = buf[i + 1];
			if (hl < 2 || i + hl + 3 > n) {
				break;
			}
			uint8_t type = buf[i + hl], len = buf[i + hl + 2];
			const uint8_t *data = buf + i + hl + 3;
			if (i + hl + 3 + len > n) {
				break;
			}
			if (type == SB_PKT_HMD_IMU) {
				handle_hmd_imu(sys, data, len, now);
				sys->n_imu++;
			} else if (type == SB_PKT_CONTROLLER) {
				handle_controller(sys, data, len, now);
				sys->n_ctrl++;
			}
			i += hl + 3 + len;
		}
		os_mutex_unlock(&sys->lock);

		if (now > next_report) {
			Q2_DEBUG("SyncBoss: %" PRIu64 " headset IMU, %" PRIu64 " controller packets", sys->n_imu,
			         sys->n_ctrl);
			if (sys->cameras != NULL) {
				struct q2_frame_stats *st = sys->stats;
				Q2_DEBUG("Cameras: frames (LED) %" PRIu64 " (%" PRIu64 ") %" PRIu64 " (%" PRIu64
				         ") %" PRIu64 " (%" PRIu64 ") %" PRIu64 " (%" PRIu64 ")",
				         st[0].frames, st[0].led_frames, st[1].frames, st[1].led_frames, st[2].frames,
				         st[2].led_frames, st[3].frames, st[3].led_frames);
			}
			next_report = now + 5 * U_TIME_1S_IN_NS;
		}

		os_thread_helper_lock(&sys->thread);
	}
	os_thread_helper_unlock(&sys->thread);
	return NULL;
}

static void
q2_frame_stats_push(struct xrt_frame_sink *sink, struct xrt_frame *xf)
{
	struct q2_frame_stats *st = (struct q2_frame_stats *)sink;
	uint64_t sum = 0;
	for (uint32_t y = 0; y < xf->height; y += 16) {
		for (uint32_t x = 0; x < xf->width; x += 16) {
			sum += xf->data[y * xf->stride + x];
		}
	}
	uint64_t mean = sum / ((xf->height / 16) * (xf->width / 16));
	st->frames++;
	/* Debug: QUEST2_DUMP_FRAMES=<dir> saves a few frames per camera as PGM. */
	const char *dump = debug_get_option_q2_dump_frames();
	if (dump != NULL && st->frames % 25 == 0 && st->frames <= 200) {
		char path[256];
		snprintf(path, sizeof(path), "%s/cam%d_%03u_mean%u.pgm", dump, st->cam, (unsigned)(st->frames / 25),
		         (unsigned)mean);
		FILE *f = fopen(path, "wb");
		if (f != NULL) {
			fprintf(f, "P5\n%u %u\n255\n", xf->width, xf->height);
			for (uint32_t y = 0; y < xf->height; y++) {
				fwrite(xf->data + y * xf->stride, 1, xf->width, f);
			}
			fclose(f);
		}
	}
	if (mean < 16) {
		st->led_frames++;
	}
}

static int
q2_sb_send_packet(void *ctx, const uint8_t *pkt, uint32_t len)
{
	struct q2_system *sys = ctx;
	if (len < 3 || len < 3u + pkt[2]) {
		return -1;
	}
	return sb_send(sys, pkt[0], pkt + 3, pkt[2]);
}

static void
q2_system_unref(struct q2_system *sys)
{
	if (--sys->refs > 0) {
		return;
	}
	q2_cameras_stop(&sys->cameras);
	os_thread_helper_destroy(&sys->thread);
	close(sys->stream_fd);
	close(sys->dev_fd);
	os_mutex_destroy(&sys->lock);
	free(sys);
}


/*
 *
 * Controllers.
 *
 */

static struct xrt_binding_input_pair simple_inputs[4] = {
    {XRT_INPUT_SIMPLE_SELECT_CLICK, XRT_INPUT_TOUCH_TRIGGER_VALUE},
    {XRT_INPUT_SIMPLE_MENU_CLICK, XRT_INPUT_TOUCH_MENU_CLICK},
    {XRT_INPUT_SIMPLE_GRIP_POSE, XRT_INPUT_TOUCH_GRIP_POSE},
    {XRT_INPUT_SIMPLE_AIM_POSE, XRT_INPUT_TOUCH_AIM_POSE},
};

static struct xrt_binding_output_pair simple_outputs[1] = {
    {XRT_OUTPUT_NAME_SIMPLE_VIBRATION, XRT_OUTPUT_NAME_TOUCH_HAPTIC},
};

static struct xrt_binding_profile binding_profiles[1] = {{
    .name = XRT_DEVICE_SIMPLE_CONTROLLER,
    .inputs = simple_inputs,
    .input_count = ARRAY_SIZE(simple_inputs),
    .outputs = simple_outputs,
    .output_count = ARRAY_SIZE(simple_outputs),
}};

static xrt_result_t
q2_ctrl_update_inputs(struct xrt_device *xdev)
{
	struct q2_controller *c = (struct q2_controller *)xdev;
	struct xrt_input *in = c->base.inputs;
	int64_t now = os_monotonic_get_ns();

	os_mutex_lock(&c->sys->lock);
	uint8_t b = c->buttons;
	/* bit0 A/X, bit1 B/Y, bit2 stick click, bit3 Meta (right) / Menu (left) */
	in[Q2_A_CLICK].value.boolean = (b & 1) != 0;
	in[Q2_B_CLICK].value.boolean = (b & 2) != 0;
	in[Q2_THUMBSTICK_CLICK].value.boolean = (b & 4) != 0;
	in[Q2_SYSTEM_CLICK].value.boolean = (b & 8) != 0;
	/* Capacitive touch (record 0x45) is not decoded yet: report presses. */
	in[Q2_A_TOUCH].value.boolean = (b & 1) != 0;
	in[Q2_B_TOUCH].value.boolean = (b & 2) != 0;
	in[Q2_TRIGGER_VALUE].value.vec1.x = c->trigger;
	in[Q2_TRIGGER_TOUCH].value.boolean = c->trigger > 0.05f;
	in[Q2_SQUEEZE_VALUE].value.vec1.x = c->grip;
	in[Q2_THUMBSTICK].value.vec2 = c->stick;
	in[Q2_THUMBSTICK_TOUCH].value.boolean = fabsf(c->stick.x) > 0.05f || fabsf(c->stick.y) > 0.05f;
	os_mutex_unlock(&c->sys->lock);

	for (int i = 0; i < Q2_INPUT_COUNT; i++) {
		in[i].timestamp = now;
	}
	return XRT_SUCCESS;
}

static xrt_result_t
q2_ctrl_get_tracked_pose(struct xrt_device *xdev,
                         enum xrt_input_name name,
                         int64_t at_timestamp_ns,
                         struct xrt_space_relation *out_relation)
{
	struct q2_controller *c = (struct q2_controller *)xdev;

	if (name != XRT_INPUT_TOUCH_AIM_POSE && name != XRT_INPUT_TOUCH_GRIP_POSE) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	struct xrt_relation_chain xrc = {0};

	/* Grip/aim correction relative to the IMU, as for Rift S Touch. */
	struct xrt_pose correction = XRT_POSE_IDENTITY;
	struct xrt_vec3 axis = {1.0f, 0, 0};
	math_quat_from_angle_vector(DEG_TO_RAD(40), &axis, &correction.orientation);
	m_relation_chain_push_pose(&xrc, &correction);

	struct xrt_space_relation *rel = m_relation_chain_reserve(&xrc);
	os_mutex_lock(&c->sys->lock);
	*rel = (struct xrt_space_relation)XRT_SPACE_RELATION_ZERO;
	rel->pose.orientation = c->fusion.rot;
	/* No positional tracking yet: hold the hands in front of the body. */
	rel->pose.position = (struct xrt_vec3){c->left ? -0.2f : 0.2f, 1.3f, -0.4f};
	math_quat_rotate_derivative(&rel->pose.orientation, &c->fusion.last.gyro, &rel->angular_velocity);
	os_mutex_unlock(&c->sys->lock);
	rel->relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	    XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);

	m_relation_chain_resolve(&xrc, out_relation);
	return XRT_SUCCESS;
}

static xrt_result_t
q2_ctrl_set_output(struct xrt_device *xdev, enum xrt_output_name name, const struct xrt_output_value *value)
{
	/* Haptics: not implemented yet (SyncBoss 0x8f commands to the controller). */
	return XRT_SUCCESS;
}

static void
q2_ctrl_destroy(struct xrt_device *xdev)
{
	struct q2_controller *c = (struct q2_controller *)xdev;

	os_mutex_lock(&c->sys->lock);
	c->sys->ctrl[c->left ? 0 : 1] = NULL;
	os_mutex_unlock(&c->sys->lock);
	q2_system_unref(c->sys);

	u_var_remove_root(c);
	m_imu_3dof_close(&c->fusion);
	u_device_free(&c->base);
}

#define SET_INPUT(c, IDX, NAME) ((c)->base.inputs[IDX].name = XRT_INPUT_TOUCH_##NAME)

static struct q2_controller *
q2_ctrl_create(struct q2_system *sys, bool left)
{
	struct q2_controller *c = U_DEVICE_ALLOCATE(struct q2_controller, U_DEVICE_ALLOC_TRACKING_NONE,
	                                            Q2_INPUT_COUNT, 1);
	if (c == NULL) {
		return NULL;
	}
	c->sys = sys;
	c->left = left;
	m_imu_3dof_init(&c->fusion, M_IMU_3DOF_USE_GRAVITY_DUR_20MS);

	u_device_populate_function_pointers(&c->base, q2_ctrl_get_tracked_pose, q2_ctrl_destroy);
	c->base.update_inputs = q2_ctrl_update_inputs;
	c->base.set_output = q2_ctrl_set_output;
	c->base.name = XRT_DEVICE_TOUCH_CONTROLLER;
	c->base.device_type = left ? XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER : XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER;
	snprintf(c->base.str, XRT_DEVICE_NAME_LEN, "Quest 2 %s Touch Controller", left ? "Left" : "Right");
	snprintf(c->base.serial, XRT_DEVICE_NAME_LEN, "%s Controller", left ? "Left" : "Right");

	if (left) {
		SET_INPUT(c, Q2_X_CLICK, X_CLICK);
		SET_INPUT(c, Q2_X_TOUCH, X_TOUCH);
		SET_INPUT(c, Q2_Y_CLICK, Y_CLICK);
		SET_INPUT(c, Q2_Y_TOUCH, Y_TOUCH);
		SET_INPUT(c, Q2_MENU_CLICK, MENU_CLICK);
	} else {
		SET_INPUT(c, Q2_A_CLICK, A_CLICK);
		SET_INPUT(c, Q2_A_TOUCH, A_TOUCH);
		SET_INPUT(c, Q2_B_CLICK, B_CLICK);
		SET_INPUT(c, Q2_B_TOUCH, B_TOUCH);
		SET_INPUT(c, Q2_SYSTEM_CLICK, SYSTEM_CLICK);
	}
	SET_INPUT(c, Q2_SQUEEZE_VALUE, SQUEEZE_VALUE);
	SET_INPUT(c, Q2_TRIGGER_TOUCH, TRIGGER_TOUCH);
	SET_INPUT(c, Q2_TRIGGER_VALUE, TRIGGER_VALUE);
	SET_INPUT(c, Q2_THUMBSTICK_CLICK, THUMBSTICK_CLICK);
	SET_INPUT(c, Q2_THUMBSTICK_TOUCH, THUMBSTICK_TOUCH);
	SET_INPUT(c, Q2_THUMBSTICK, THUMBSTICK);
	SET_INPUT(c, Q2_THUMBREST_TOUCH, THUMBREST_TOUCH);
	SET_INPUT(c, Q2_GRIP_POSE, GRIP_POSE);
	SET_INPUT(c, Q2_AIM_POSE, AIM_POSE);
	c->base.outputs[0].name = XRT_OUTPUT_NAME_TOUCH_HAPTIC;
	c->base.binding_profiles = binding_profiles;
	c->base.binding_profile_count = ARRAY_SIZE(binding_profiles);
	c->base.supported.orientation_tracking = true;

	u_var_add_root(c, c->base.str, true);
	m_imu_3dof_add_vars(&c->fusion, c, "");
	return c;
}


/*
 *
 * Headset.
 *
 */

static xrt_result_t
q2_hmd_get_tracked_pose(struct xrt_device *xdev,
                        enum xrt_input_name name,
                        int64_t at_timestamp_ns,
                        struct xrt_space_relation *out_relation)
{
	struct q2_hmd *hmd = (struct q2_hmd *)xdev;

	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
	m_relation_history_get(hmd->relation_hist, at_timestamp_ns, &rel);
	struct xrt_quat *q = &rel.pose.orientation;
	if (rel.relation_flags == 0 || q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w < 0.5f) {
		rel.pose = (struct xrt_pose){XRT_QUAT_IDENTITY, {0, 1.6f, 0}};
		rel.relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
		                                                     XRT_SPACE_RELATION_POSITION_VALID_BIT);
	}
	math_quat_normalize(&rel.pose.orientation);
	*out_relation = rel;
	return XRT_SUCCESS;
}

static xrt_result_t
q2_hmd_get_visibility_mask(struct xrt_device *xdev,
                           enum xrt_visibility_mask_type type,
                           uint32_t view_index,
                           struct xrt_visibility_mask **out_mask)
{
	struct xrt_fov fov = xdev->hmd->distortion.fov[view_index];
	u_visibility_mask_get_default(type, &fov, out_mask);
	return XRT_SUCCESS;
}

static void
q2_hmd_destroy(struct xrt_device *xdev)
{
	struct q2_hmd *hmd = (struct q2_hmd *)xdev;

	os_mutex_lock(&hmd->sys->lock);
	hmd->sys->hmd = NULL;
	os_mutex_unlock(&hmd->sys->lock);
	q2_system_unref(hmd->sys);

	u_var_remove_root(hmd);
	m_imu_3dof_close(&hmd->fusion);
	m_relation_history_destroy(&hmd->relation_hist);
	u_device_free(&hmd->base);
}

static struct q2_hmd *
q2_hmd_create(struct q2_system *sys)
{
	enum u_device_alloc_flags flags = (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE);
	struct q2_hmd *hmd = U_DEVICE_ALLOCATE(struct q2_hmd, flags, 1, 0);
	if (hmd == NULL) {
		return NULL;
	}
	hmd->sys = sys;
	m_relation_history_create(&hmd->relation_hist);
	m_imu_3dof_init(&hmd->fusion, M_IMU_3DOF_USE_GRAVITY_DUR_20MS);

	u_device_populate_function_pointers(&hmd->base, q2_hmd_get_tracked_pose, q2_hmd_destroy);
	hmd->base.get_view_poses = u_device_get_view_poses;
	hmd->base.get_visibility_mask = q2_hmd_get_visibility_mask;
	hmd->base.name = XRT_DEVICE_GENERIC_HMD;
	hmd->base.device_type = XRT_DEVICE_TYPE_HMD;
	hmd->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;
	hmd->base.supported.orientation_tracking = true;
	hmd->base.supported.position_tracking = false;
	snprintf(hmd->base.str, XRT_DEVICE_NAME_LEN, "Meta Quest 2");
	snprintf(hmd->base.serial, XRT_DEVICE_NAME_LEN, "Quest 2");

	hmd->base.hmd->blend_modes[0] = XRT_BLEND_MODE_OPAQUE;
	hmd->base.hmd->blend_mode_count = 1;
	/* gamescope drives the panel at 120 Hz; match it to avoid judder. */
	hmd->base.hmd->screens[0].nominal_frame_interval_ns = time_s_to_ns(1.0f / 120.0f);

	/*
	 * One logical side-by-side screen: each eye 1832x1920 (the panel half,
	 * unrotated); gamescope (GAMESCOPE_QKX_SBS) puts each half on its eye.
	 */
	const int eye_w = 1832, eye_h = 1920;
	hmd->base.hmd->screens[0].w_pixels = eye_w * 2;
	hmd->base.hmd->screens[0].h_pixels = eye_h;
	for (int eye = 0; eye < 2; eye++) {
		hmd->base.hmd->views[eye].display.w_pixels = eye_w;
		hmd->base.hmd->views[eye].display.h_pixels = eye_h;
		hmd->base.hmd->views[eye].viewport.x_pixels = eye * eye_w;
		hmd->base.hmd->views[eye].viewport.y_pixels = 0;
		hmd->base.hmd->views[eye].viewport.w_pixels = eye_w;
		hmd->base.hmd->views[eye].viewport.h_pixels = eye_h;
		hmd->base.hmd->views[eye].rot = u_device_rotation_ident;
	}

	/* Approximate Quest 2 per-eye field of view; lens model to come. */
	const double h_fov = 97.0 * M_PI / 180.0, v_fov = 93.0 * M_PI / 180.0;
	math_compute_fovs(1, 0.45, h_fov, 1, 0.5, v_fov, &hmd->base.hmd->distortion.fov[0]);
	math_compute_fovs(1, 0.55, h_fov, 1, 0.5, v_fov, &hmd->base.hmd->distortion.fov[1]);
	u_distortion_mesh_set_none(&hmd->base);

	u_var_add_root(hmd, "Quest 2", true);
	m_imu_3dof_add_vars(&hmd->fusion, hmd, "");
	return hmd;
}


/*
 *
 * Interface.
 *
 */

bool
q2_present(void)
{
	return access(SB_DEV, R_OK | W_OK) == 0 && access(SB_STREAM, R_OK) == 0;
}

int
q2_create_devices(struct xrt_device **out_hmd, struct xrt_device **out_left, struct xrt_device **out_right)
{
	q2_log_level = debug_get_log_option_q2_log();

	/* Proximity calibration files are Android-only: do not stall on them. */
	int tfd = open("/sys/class/firmware/timeout", O_WRONLY);
	if (tfd >= 0) {
		(void)!write(tfd, "1", 1);
		close(tfd);
	}
	int lfd = open(SB_SYSFS, O_WRONLY);
	if (lfd >= 0) {
		(void)!write(lfd, "512", 3);
		close(lfd);
	}

	struct q2_system *sys = U_TYPED_CALLOC(struct q2_system);
	sys->dev_fd = open(SB_DEV, O_RDWR);
	sys->stream_fd = open(SB_STREAM, O_RDONLY | O_NONBLOCK);
	if (sys->dev_fd < 0 || sys->stream_fd < 0) {
		Q2_ERROR("Cannot open SyncBoss: %s", strerror(errno));
		if (sys->dev_fd >= 0) {
			close(sys->dev_fd);
		}
		if (sys->stream_fd >= 0) {
			close(sys->stream_fd);
		}
		free(sys);
		return -1;
	}
	os_mutex_init(&sys->lock);
	os_thread_helper_init(&sys->thread);

	sys->hmd = q2_hmd_create(sys);
	sys->ctrl[0] = q2_ctrl_create(sys, true);
	sys->ctrl[1] = q2_ctrl_create(sys, false);
	sys->refs = 3;

	/* Give SyncBoss time to wake, then: headset IMU on, controller radio on. */
	os_nanosleep(U_TIME_1MS_IN_NS * 500);
	const uint8_t zero = 0;
	sb_send(sys, 110, &zero, 1);
	os_nanosleep(U_TIME_1MS_IN_NS * 50);
	sb_send(sys, 133, &zero, 1);

	os_thread_helper_start(&sys->thread, q2_run, sys);
	Q2_INFO("Quest 2: SyncBoss streaming (headset IMU, Touch controllers)");

	/* Tracking cameras: replay the recorded start-up if a script exists. */
	struct xrt_frame_sink *sinks[Q2_CAMERA_COUNT];
	for (int i = 0; i < Q2_CAMERA_COUNT; i++) {
		sys->stats[i].base.push_frame = q2_frame_stats_push;
		sys->stats[i].cam = i;
		sinks[i] = &sys->stats[i].base;
	}
	sys->cameras = q2_cameras_start(debug_get_option_q2_camera_script(), q2_sb_send_packet, sys, sinks);

	*out_hmd = &sys->hmd->base;
	*out_left = &sys->ctrl[0]->base;
	*out_right = &sys->ctrl[1]->base;
	return 0;
}
