// SPDX-License-Identifier: GPL-2.0
/*
 * qkx_camtrace: record how Android's camera userspace drives the Qualcomm
 * Spectra camera driver, so the same sequence can be replayed on Linux.
 *
 * For each camera V4L2 node (/dev/video0 = cam-req-mgr, /dev/video1 =
 * cam_sync, /dev/v4l-subdev*), the video_device's fops pointer is swapped for
 * a copy whose unlocked_ioctl logs the call before and after running the
 * original handler. VIDIOC_CAM_CONTROL arguments are copied whole; for
 * CAM_CONFIG_DEV the referenced cam_packet and every command buffer it points
 * at are copied from the camera memory manager.
 *
 * Records go to a vmalloc ring read through /proc/qkx_camtrace (record format:
 * struct qct_rec + payload). Unloading the module restores the original fops.
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <media/v4l2-dev.h>
#include <media/cam_defs.h>
#include "cam_mem_mgr_api.h"

#define QCT_MAGIC 0x31544351 /* "QCT1" */
#define QCT_MAX_NODES 40
#define QCT_ARG_MAX (64 * 1024)
#define QCT_BUF_MAX (512 * 1024)
#define QCT_REPEAT_LIMIT 400

enum { QCT_PRE, QCT_POST, QCT_PACKET, QCT_CMDBUF, QCT_RAW_PRE, QCT_RAW_POST,
       QCT_PTR, QCT_PTR_POST };
#define QCT_PTR_MAX 4096
#define QCT_MAX_PTRS 8

struct qct_rec {
	u32 magic;
	u32 len;	/* payload bytes following this header */
	u64 ts_ns;
	u32 pid;
	u32 minor;	/* video device minor (node) */
	u32 cmd;	/* ioctl cmd */
	u32 phase;	/* QCT_* */
	s64 ret;	/* POST: return value; CMDBUF: mem handle; PACKET: handle */
	u64 aux;	/* CMDBUF: offset in the buffer; PACKET: offset */
} __packed;

#ifndef QCT_PROC
#define QCT_PROC "qkx_camtrace"
#endif

/*
 * cam-req-mgr (/dev/video0) and cam_sync (/dev/video1) allow a single open,
 * which the camera HAL already holds: reach them through V4L2's
 * video_devices[] table instead (address from /proc/kallsyms).
 */
static unsigned long vdev_table;
module_param(vdev_table, ulong, 0444);
MODULE_PARM_DESC(vdev_table, "address of video_devices[] (hook minors 0 and 1 through it)");
static bool table_only;
module_param(table_only, bool, 0444);
MODULE_PARM_DESC(table_only, "hook only the nodes reached through vdev_table");

static uint only_op;
module_param(only_op, uint, 0444);
MODULE_PARM_DESC(only_op, "log only this VIDIOC_CAM_CONTROL opcode (0 = everything)");

static bool copy_any;
module_param(copy_any, bool, 0444);
MODULE_PARM_DESC(copy_any, "copy the argument whatever handle_type says (cam-req-mgr passes 0)");

static unsigned long ring_mb = 64;
module_param(ring_mb, ulong, 0444);
MODULE_PARM_DESC(ring_mb, "log size in MiB");

static char *ring;
static size_t ring_size, ring_pos;
static bool ring_full;
static DEFINE_MUTEX(ring_lock);

struct qct_node {
	struct video_device *vdev;
	const struct v4l2_file_operations *orig;
	struct v4l2_file_operations ops;
	unsigned int counts[64];
};
static struct qct_node nodes[QCT_MAX_NODES];
static int num_nodes;

static void qct_log(u32 minor, u32 cmd, u32 phase, s64 ret, u64 aux,
		    const void *a, size_t alen, const void *b, size_t blen)
{
	struct qct_rec r = {
		.magic = QCT_MAGIC, .len = alen + blen, .ts_ns = ktime_get_ns(),
		.pid = current->pid, .minor = minor, .cmd = cmd, .phase = phase,
		.ret = ret, .aux = aux,
	};

	mutex_lock(&ring_lock);
	if (ring_pos + sizeof(r) + alen + blen > ring_size) {
		ring_full = true;
		goto out;
	}
	memcpy(ring + ring_pos, &r, sizeof(r));
	ring_pos += sizeof(r);
	if (alen) {
		memcpy(ring + ring_pos, a, alen);
		ring_pos += alen;
	}
	if (blen) {
		memcpy(ring + ring_pos, b, blen);
		ring_pos += blen;
	}
out:
	mutex_unlock(&ring_lock);
}

/* Copy a region of a camera memory-manager buffer into the log. */
static void qct_log_mem(u32 minor, u32 cmd, u32 phase, s32 handle, u64 off, u64 len)
{
	uintptr_t va = 0;
	size_t blen = 0;

	if (cam_mem_get_cpu_buf(handle, &va, &blen) || !va)
		return;
	if (off < blen) {
		len = min_t(u64, len ? len : blen - off, blen - off);
		len = min_t(u64, len, QCT_BUF_MAX);
		qct_log(minor, cmd, phase, handle, off, (void *)(va + off), len, NULL, 0);
	}
	cam_mem_put_cpu_buf(handle);
}

static void qct_log_packet_at(u32 minor, u32 cmd, s32 handle, u64 offset)
{
	uintptr_t va = 0;
	size_t blen = 0;
	struct cam_packet *pkt;
	struct cam_cmd_buf_desc *desc;
	u32 i, psize;

	if (cam_mem_get_cpu_buf(handle, &va, &blen) || !va)
		return;
	if (offset + sizeof(*pkt) > blen)
		goto put;
	pkt = (struct cam_packet *)(va + offset);
	psize = min_t(u64, pkt->header.size, blen - offset);
	qct_log(minor, cmd, QCT_PACKET, handle, offset, pkt, psize, NULL, 0);
	if (pkt->cmd_buf_offset + (u64)pkt->num_cmd_buf * sizeof(*desc) >
	    psize - offsetof(struct cam_packet, payload))
		goto put;
	desc = (struct cam_cmd_buf_desc *)((u8 *)pkt->payload + pkt->cmd_buf_offset);
	for (i = 0; i < pkt->num_cmd_buf && i < 32; i++)
		if (desc[i].mem_handle > 0 && desc[i].length)
			qct_log_mem(minor, cmd, QCT_CMDBUF, desc[i].mem_handle,
				    desc[i].offset, desc[i].length);
put:
	cam_mem_put_cpu_buf(handle);
}

static void qct_log_packet(u32 minor, u32 cmd, const struct cam_config_dev_cmd *cfg)
{
	qct_log_packet_at(minor, cmd, cfg->packet_handle, cfg->offset);
}

/* SET_STREAM_MODE: every registered image carries its own packet. */
static void qct_log_stream_mode(u32 minor, u32 cmd, const void *data, u32 sz)
{
	const struct cam_set_stream_mode *m = data;
	u32 i;

	if (sz < offsetof(struct cam_set_stream_mode, stream_images))
		return;
	for (i = 0; i < m->num_images && i < CAM_MAX_STREAM_MODE_HANDLES; i++) {
		if (offsetof(struct cam_set_stream_mode, stream_images[i + 1]) > sz)
			break;
		if (m->stream_images[i].packet_handle)
			qct_log_packet_at(minor, cmd, m->stream_images[i].packet_handle,
					  m->stream_images[i].packet_offset);
	}
}

/*
 * Arguments can point at further user structures (capability buffers, ISP
 * acquire resources). Android tags heap pointers in the top byte, so test the
 * untagged value. Copy up to QCT_PTR_MAX bytes, stopping at an unmapped page.
 */
static bool qct_is_uptr(u64 v)
{
	v &= 0x00ffffffffffffffULL;
	return v >= 0x1000000000ULL && v < 0x8000000000ULL;
}

static void qct_log_ptrs(u32 minor, u32 cmd, u32 phase, const void *data, u32 sz)
{
	u32 i, n = 0;
	void *buf;

	if (!data)
		return;
	buf = kmalloc(QCT_PTR_MAX, GFP_KERNEL);
	if (!buf)
		return;
	for (i = 0; i + 8 <= sz && n < QCT_MAX_PTRS; i += 8) {
		u64 v = *(const u64 *)(data + i);
		u32 len = 0, chunk;

		if (!qct_is_uptr(v))
			continue;
		while (len < QCT_PTR_MAX) {
			chunk = min_t(u32, QCT_PTR_MAX - len,
				      PAGE_SIZE - ((v + len) & (PAGE_SIZE - 1)));
			if (copy_from_user(buf + len, u64_to_user_ptr(v + len), chunk))
				break;
			len += chunk;
		}
		if (len) {
			qct_log(minor, cmd, phase, i, v, buf, len, NULL, 0);
			n++;
		}
	}
	kfree(buf);
}

static struct qct_node *qct_find(struct file *f)
{
	struct video_device *vdev = video_devdata(f);
	int i;

	for (i = 0; i < num_nodes; i++)
		if (nodes[i].vdev == vdev)
			return &nodes[i];
	return NULL;
}

static long qct_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct qct_node *n = qct_find(f);
	void __user *uarg = (void __user *)arg;
	struct cam_control ctl;
	void *data = NULL;
	u32 minor;
	long ret;

	if (!n)
		return -ENOTTY;
	minor = n->vdev->minor;

	if (only_op) {
		if (cmd != VIDIOC_CAM_CONTROL || copy_from_user(&ctl, uarg, sizeof(ctl)) ||
		    ctl.op_code != only_op)
			return n->orig->unlocked_ioctl(f, cmd, arg);
	}

	if (cmd == VIDIOC_CAM_CONTROL && !copy_from_user(&ctl, uarg, sizeof(ctl))) {
		u32 sz = (ctl.handle_type == CAM_HANDLE_USER_POINTER || copy_any) ?
			 min_t(u32, ctl.size, QCT_ARG_MAX) : 0;

		if (sz) {
			data = kvmalloc(sz, GFP_KERNEL);
			if (data && copy_from_user(data, u64_to_user_ptr(ctl.handle), sz))
				sz = 0;
		}
		/* Everything the call reads from memory is logged before the call. */
		if (data && sz)
			qct_log_ptrs(minor, cmd, QCT_PTR, data, sz);
		if (ctl.handle_type == CAM_HANDLE_MEM_HANDLE)
			qct_log_packet_at(minor, cmd, ctl.handle, 0);
		if (ctl.op_code == CAM_CONFIG_DEV && data && sz >= sizeof(struct cam_config_dev_cmd))
			qct_log_packet(minor, cmd, data);
		if (ctl.op_code == CAM_SET_STREAM_MODE && data)
			qct_log_stream_mode(minor, cmd, data, sz);
		qct_log(minor, cmd, QCT_PRE, 0, 0, &ctl, sizeof(ctl), data, data ? sz : 0);

		ret = n->orig->unlocked_ioctl(f, cmd, arg);

		if (data && sz)
			qct_log_ptrs(minor, cmd, QCT_PTR_POST, data, sz);
		if (data && sz && !copy_from_user(data, u64_to_user_ptr(ctl.handle), sz))
			qct_log(minor, cmd, QCT_POST, ret, 0, &ctl, sizeof(ctl), data, sz);
		else
			qct_log(minor, cmd, QCT_POST, ret, 0, &ctl, sizeof(ctl), NULL, 0);
		kvfree(data);
		return ret;
	}

	/* Other ioctls (fastpath buffer queues, sync objects, V4L2 events). */
	{
		u32 sz = min_t(u32, _IOC_SIZE(cmd), 8192);
		bool show = n->counts[_IOC_NR(cmd) & 63]++ < QCT_REPEAT_LIMIT;

		if (show && sz) {
			data = kvmalloc(sz, GFP_KERNEL);
			if (data && (_IOC_DIR(cmd) & _IOC_WRITE) && copy_from_user(data, uarg, sz))
				sz = 0;
		}
		if (show)
			qct_log(minor, cmd, QCT_RAW_PRE, 0, 0,
				(_IOC_DIR(cmd) & _IOC_WRITE) ? data : NULL,
				(_IOC_DIR(cmd) & _IOC_WRITE) && data ? sz : 0, NULL, 0);
		ret = n->orig->unlocked_ioctl(f, cmd, arg);
		if (show) {
			bool rd = (_IOC_DIR(cmd) & _IOC_READ) && data && !copy_from_user(data, uarg, sz);

			qct_log(minor, cmd, QCT_RAW_POST, ret, 0, rd ? data : NULL, rd ? sz : 0, NULL, 0);
		}
		kvfree(data);
		return ret;
	}
}

static void qct_hook_vdev(struct video_device *vdev, const char *what)
{
	struct qct_node *n;

	if (vdev && vdev->fops && vdev->fops->unlocked_ioctl && num_nodes < QCT_MAX_NODES) {
		n = &nodes[num_nodes++];
		n->vdev = vdev;
		n->orig = vdev->fops;
		n->ops = *vdev->fops;
		n->ops.unlocked_ioctl = qct_ioctl;
		/* video_device keeps a pointer; v4l2_ioctl() reads it per call. */
		WRITE_ONCE(vdev->fops, &n->ops);
		pr_info(QCT_PROC ": hooked %s (%s, minor %d)\n", what, vdev->name, vdev->minor);
	}
}

static void qct_hook(const char *path)
{
	struct file *f = filp_open(path, O_RDONLY | O_NONBLOCK, 0);

	if (IS_ERR(f))
		return;
	qct_hook_vdev(video_devdata(f), path);
	filp_close(f, NULL);
}

static ssize_t qct_read(struct file *f, char __user *buf, size_t len, loff_t *pos)
{
	ssize_t n;

	mutex_lock(&ring_lock);
	if (*pos >= ring_pos) {
		n = 0;
	} else {
		n = min_t(size_t, len, ring_pos - *pos);
		if (copy_to_user(buf, ring + *pos, n))
			n = -EFAULT;
		else
			*pos += n;
	}
	mutex_unlock(&ring_lock);
	return n;
}

/* Writing anything clears the log. */
static ssize_t qct_write(struct file *f, const char __user *buf, size_t len, loff_t *pos)
{
	int i;

	mutex_lock(&ring_lock);
	ring_pos = 0;
	ring_full = false;
	for (i = 0; i < num_nodes; i++)
		memset(nodes[i].counts, 0, sizeof(nodes[i].counts));
	mutex_unlock(&ring_lock);
	return len;
}

static const struct file_operations qct_proc_fops = {
	.owner = THIS_MODULE,
	.read = qct_read,
	.write = qct_write,
	.llseek = default_llseek,
};

static int __init qct_init(void)
{
	char path[32];
	int i;

	ring_size = ring_mb << 20;
	ring = vmalloc(ring_size);
	if (!ring)
		return -ENOMEM;
	if (vdev_table) {
		struct video_device **table = (struct video_device **)vdev_table;

		qct_hook_vdev(table[0], "video_devices[0]");
		qct_hook_vdev(table[1], "video_devices[1]");
	}
	if (!table_only) {
		qct_hook("/dev/video0");
		qct_hook("/dev/video1");
		for (i = 0; i < 32; i++) {
			snprintf(path, sizeof(path), "/dev/v4l-subdev%d", i);
			qct_hook(path);
		}
	}
	if (!proc_create(QCT_PROC, 0600, NULL, &qct_proc_fops))
		pr_err(QCT_PROC ": proc_create failed\n");
	pr_info(QCT_PROC ": %d nodes, %lu MiB log\n", num_nodes, ring_mb);
	return 0;
}

static void __exit qct_exit(void)
{
	int i;

	remove_proc_entry(QCT_PROC, NULL);
	for (i = 0; i < num_nodes; i++)
		WRITE_ONCE(nodes[i].vdev->fops, nodes[i].orig);
	/* Let in-flight ioctls that already loaded our ops finish. */
	synchronize_rcu();
	msleep(200);
	vfree(ring);
}

module_init(qct_init);
module_exit(qct_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Record camera driver ioctls for replay (research)");
