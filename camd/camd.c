/*
 * camd - expose the J4's rear camera (S5K3L2, FIMC-IS2 OTF chain) as an
 * ordinary V4L2 webcam through a v4l2loopback node.
 *
 *   SS0 -> 3AA0 -> ISP0 -> MCSC0 -> M0P (NV12) --write()--> /dev/video20
 *
 * The camera is only powered/streaming while some other process holds the
 * loopback node open: a reader appearing starts the pipeline, and a few
 * seconds without readers stops it again.
 *
 * Build on the phone:  gcc -I. -O2 -o camd camd.c
 * Run:                 camd [-c rear|front] [-d /dev/video20] [-r 0|90|180|270] [-s WxH] [-v]
 *                      -s is the PUBLISHED size (after rotation), default 480x640
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#define _LINUX_TYPES_H /* header typedefs clash with libc; stdint is already in */
#include "fimc-is-metadata.h"
#include "ion.h"

#define CID_BASE		(V4L2_CTRL_CLASS_CAMERA | 0x1000)
#define CID_IS_S_STREAM		(CID_BASE + 14)
#define CID_IS_END_OF_STREAM	(CID_BASE + 58)
#define CID_IS_FORCE_DONE	(CID_BASE + 50)

/* video ids (device node = /dev/video(100 + id)) */
#define VID_30S		10
#define VID_30P		12
#define VID_I0S		30
#define VID_I0P		32
#define VID_M0S		60
#define VID_M0P		70

/*
 * Per-camera profile. The OTF chain (3AA0 -> ISP0 -> MCSC0) is shared: the
 * ischain finds its sensor by matching the module id among the opened sensor
 * nodes, so the front camera just uses ss1 and its own sensor id.
 */
struct camera {
	const char *name;
	int sensor_id;		/* module id passed to S_INPUT */
	int ss_vid;		/* sensor video node id (SS0 = 1, SS1 = 2) */
	const char *ss_sysname;	/* its name in sysfs */
	int sen_w, sen_h;	/* sensor mode, 4:3 */
	int rotation;		/* default clockwise rotation to make it upright */
};

static const struct camera cameras[] = {
	/* S5K3L2, rear: mode 3, 2072x1552 @30, 4 lanes, mounted sideways */
	{ "rear",  8, 1, "exynos-fimc-is-ss0", 2072, 1552, 90 },
	/* S5K5E3, front: mode 1, 1280x960 @30, 2 lanes */
	{ "front", 17, 2, "exynos-fimc-is-ss1", 1280, 960, 270 },
};
static const struct camera *cam = &cameras[0];

/*
 * The MCSC hardware scaler shrinks the sensor picture (cam->sen_w x sen_h) to
 * out_w x out_h (4:3, set by -s) at no CPU cost; everything downstream
 * (rotation, the readers' colour conversion) scales with the pixel count.
 */
#define OUT_FPS		30

#define SPARE_SIZE	(32 * 1024)
#define MAXBUF		4
#define NBUF		4

#define IDLE_STOP_SEC	2
#define SCAN_MS_IDLE	500
#define SCAN_MS_RUN	500

static int sen_w, sen_h;	/* active camera's sensor mode (from -c) */
static int verbose;
static int out_w = 640, out_h = 480;	/* MCSC output (sensor orientation), from -s */
static int out_size;			/* NV12 frame size = out_w * out_h * 3 / 2 */
static int rotation = -1;	/* degrees clockwise applied before publishing: 0, 90, 180, 270 */
static int pub_w, pub_h;	/* published frame size (swapped for 90/270) */
static unsigned char *rot_buf;
static volatile sig_atomic_t quit;

/* under systemd, "<5>" makes the line notice-level so journald keeps it */
static const char *log_prefix = "camd: ";
#define LOG(fmt, ...) do { fprintf(stderr, "%s" fmt "\n", log_prefix, ##__VA_ARGS__); } while (0)
#define DBG(fmt, ...) do { if (verbose) fprintf(stderr, "camd: " fmt "\n", ##__VA_ARGS__); } while (0)

struct buf {
	int fd[VIDEO_MAX_PLANES];
	unsigned int len[VIDEO_MAX_PLANES];
	void *spare;
	unsigned int spare_len;
	void *img;
	unsigned int img_len;
};

struct node {
	const char *name;
	const char *sysname;	/* name in sysfs video4linux dir */
	int vid;
	int fd;
	enum v4l2_buf_type type;
	int nbufs;
	int nplanes;
	struct buf buf[MAXBUF];
};

static struct node n_ss0 = { "ss0",  "exynos-fimc-is-ss0", 1, -1, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
static struct node n_30s = { "3aa",  "exynos-fimc-is-30s", VID_30S, -1, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
static struct node n_30p = { "3aap", "exynos-fimc-is-30p", VID_30P, -1, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
static struct node n_i0s = { "isp",  "exynos-fimc-is-i0s", VID_I0S, -1, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
static struct node n_i0p = { "ispp", "exynos-fimc-is-i0p", VID_I0P, -1, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
static struct node n_m0s = { "mcs",  "exynos-fimc-is-m0s", VID_M0S, -1, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
static struct node n_m0p = { "mcsp", "exynos-fimc-is-m0p", VID_M0P, -1, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };

static struct node *all_nodes[] = {
	&n_ss0, &n_30s, &n_30p, &n_i0s, &n_i0p, &n_m0s, &n_m0p,
};
#define NNODES (sizeof(all_nodes) / sizeof(all_nodes[0]))

static int ion_fd = -1;
static int stream_on;		/* sensor front-start issued */

/* ------------------------------------------------------------------ */
/* ION buffers                                                         */
/* ------------------------------------------------------------------ */

/*
 * system-heap ION buffer -> dma-buf fd. Buffers the CPU only reads (the
 * MCSC output image) are cached, since uncached reads are ~50x slower; the
 * caller syncs them before each read (ion_sync_fd). Shot-metadata buffers
 * stay uncached so the kernel always sees what userspace wrote.
 */
static int ion_alloc_fd(size_t len, int cached)
{
	struct ion_allocation_data a = {
		.len = len, .align = 4096, .heap_id_mask = 1 /* system */,
		.flags = cached ? ION_FLAG_CACHED : 0,
	};
	struct ion_fd_data d;
	struct ion_handle_data h;

	if (ion_fd < 0) {
		ion_fd = open("/dev/ion", O_RDWR | O_CLOEXEC);
		if (ion_fd < 0) {
			LOG("open /dev/ion: %s", strerror(errno));
			return -1;
		}
	}
	if (ioctl(ion_fd, ION_IOC_ALLOC, &a)) {
		LOG("ION_IOC_ALLOC(%zu): %s", len, strerror(errno));
		return -1;
	}
	d.handle = a.handle;
	d.fd = -1;
	if (ioctl(ion_fd, ION_IOC_SHARE, &d)) {
		LOG("ION_IOC_SHARE: %s", strerror(errno));
		h.handle = a.handle;
		ioctl(ion_fd, ION_IOC_FREE, &h);
		return -1;
	}
	h.handle = a.handle;
	ioctl(ion_fd, ION_IOC_FREE, &h);	/* the dma-buf fd keeps it alive */
	return d.fd;
}

/* make DMA-written data in a cached buffer visible to the CPU */
static void ion_sync_fd(int fd)
{
	struct ion_fd_data d = { .handle = 0, .fd = fd };

	if (ion_fd >= 0 && ioctl(ion_fd, ION_IOC_SYNC, &d))
		DBG("ION_IOC_SYNC: %s", strerror(errno));
}

static void node_free_bufs(struct node *n)
{
	int i, p;

	for (i = 0; i < MAXBUF; i++) {
		struct buf *b = &n->buf[i];

		if (b->spare)
			munmap(b->spare, b->spare_len);
		if (b->img)
			munmap(b->img, b->img_len);
		for (p = 0; p < VIDEO_MAX_PLANES; p++)
			if (b->fd[p] > 0)
				close(b->fd[p]);
		memset(b, 0, sizeof(*b));
	}
	n->nbufs = 0;
}

/* ------------------------------------------------------------------ */
/* V4L2 helpers                                                        */
/* ------------------------------------------------------------------ */

/*
 * The FIMC-IS nodes answer VIDIOC_ENUM_FMT with success forever, which
 * makes format enumerators (GStreamer's device provider, hence Cheese)
 * loop until memory runs out. A udev rule therefore removes them from /dev,
 * and we make private nodes under /dev/camd (devtmpfs; /run is nodev) from the sysfs dev numbers.
 */
static int node_open(struct node *n)
{
	char path[64], buf[64];
	DIR *d;
	struct dirent *e;
	int major = -1, minor = -1;

	d = opendir("/sys/class/video4linux");
	if (!d) {
		LOG("opendir /sys/class/video4linux: %s", strerror(errno));
		return -1;
	}
	while ((e = readdir(d))) {
		FILE *f;
		char name[64] = "";

		if (strncmp(e->d_name, "video", 5))
			continue;
		snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name", e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(name, sizeof(name), f))
			name[strcspn(name, "\n")] = 0;
		fclose(f);
		if (strcmp(name, n->sysname))
			continue;
		snprintf(path, sizeof(path), "/sys/class/video4linux/%s/dev", e->d_name);
		f = fopen(path, "r");
		if (f) {
			if (fscanf(f, "%d:%d", &major, &minor) != 2)
				major = minor = -1;
			fclose(f);
		}
		break;
	}
	closedir(d);
	if (major < 0) {
		LOG("%s: no video device named %s", n->name, n->sysname);
		return -1;
	}

	mkdir("/dev/camd", 0700);
	snprintf(buf, sizeof(buf), "/dev/camd/%s", n->sysname);
	unlink(buf);
	if (mknod(buf, S_IFCHR | 0600, makedev(major, minor))) {
		LOG("mknod %s: %s", buf, strerror(errno));
		return -1;
	}
	n->fd = open(buf, O_RDWR | O_CLOEXEC);
	if (n->fd < 0) {
		LOG("open %s (%s): %s", buf, n->name, strerror(errno));
		return -1;
	}
	return 0;
}

static void node_close(struct node *n)
{
	node_free_bufs(n);
	if (n->fd >= 0)
		close(n->fd);
	n->fd = -1;
}

static int s_ctrl(struct node *n, unsigned int id, int value, const char *what)
{
	struct v4l2_control c = { .id = id, .value = value };

	if (ioctl(n->fd, VIDIOC_S_CTRL, &c)) {
		LOG("S_CTRL %s %s=%d: %s", n->name, what, value, strerror(errno));
		return -1;
	}
	return 0;
}

static int s_input(struct node *n, unsigned int input)
{
	if (ioctl(n->fd, VIDIOC_S_INPUT, &input)) {
		LOG("S_INPUT %s 0x%08x: %s", n->name, input, strerror(errno));
		return -1;
	}
	return 0;
}

/* S_INPUT layout: stream[25:24] module[23:16] vindex[15:8] intype[7:4] leader[3:0] */
static unsigned int mk_input(int module, int vindex, int intype, int leader)
{
	return (module << 16) | (vindex << 8) | (intype << 4) | leader;
}

static int s_fmt(struct node *n, unsigned int pixfmt, int w, int h)
{
	struct v4l2_format f;

	memset(&f, 0, sizeof(f));
	f.type = n->type;
	f.fmt.pix_mp.width = w;
	f.fmt.pix_mp.height = h;
	f.fmt.pix_mp.pixelformat = pixfmt;
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	f.fmt.pix_mp.num_planes = 1;
	if (ioctl(n->fd, VIDIOC_S_FMT, &f)) {
		LOG("S_FMT %s %dx%d: %s", n->name, w, h, strerror(errno));
		return -1;
	}
	return 0;
}

static int reqbufs_dmabuf(struct node *n, int count, int map_img)
{
	struct v4l2_requestbuffers rb;
	int i, p;

	memset(&rb, 0, sizeof(rb));
	rb.count = count;
	rb.type = n->type;
	rb.memory = V4L2_MEMORY_DMABUF;
	if (ioctl(n->fd, VIDIOC_REQBUFS, &rb)) {
		LOG("REQBUFS %s: %s", n->name, strerror(errno));
		return -1;
	}
	n->nbufs = rb.count > MAXBUF ? MAXBUF : rb.count;

	for (i = 0; i < n->nbufs; i++) {
		struct v4l2_plane planes[VIDEO_MAX_PLANES];
		struct v4l2_buffer b;

		memset(planes, 0, sizeof(planes));
		memset(&b, 0, sizeof(b));
		b.type = n->type;
		b.memory = V4L2_MEMORY_DMABUF;
		b.index = i;
		b.m.planes = planes;
		b.length = VIDEO_MAX_PLANES;
		if (ioctl(n->fd, VIDIOC_QUERYBUF, &b)) {
			LOG("QUERYBUF %s #%d: %s", n->name, i, strerror(errno));
			return -1;
		}
		n->nplanes = b.length;
		for (p = 0; p < (int)b.length; p++) {
			n->buf[i].len[p] = planes[p].length;
			/* only the image plane of a CPU-read node is cached */
			n->buf[i].fd[p] = ion_alloc_fd(planes[p].length, map_img && p == 0);
			if (n->buf[i].fd[p] < 0)
				return -1;
		}
		/* the last plane is the shot-metadata spare plane */
		p = b.length - 1;
		n->buf[i].spare_len = n->buf[i].len[p];
		n->buf[i].spare = mmap(NULL, n->buf[i].spare_len, PROT_READ | PROT_WRITE,
				       MAP_SHARED, n->buf[i].fd[p], 0);
		if (n->buf[i].spare == MAP_FAILED) {
			LOG("mmap spare %s #%d: %s", n->name, i, strerror(errno));
			n->buf[i].spare = NULL;
			return -1;
		}
		memset(n->buf[i].spare, 0, n->buf[i].spare_len);
		if (map_img) {
			n->buf[i].img_len = n->buf[i].len[0];
			n->buf[i].img = mmap(NULL, n->buf[i].img_len, PROT_READ | PROT_WRITE,
					     MAP_SHARED, n->buf[i].fd[0], 0);
			if (n->buf[i].img == MAP_FAILED) {
				LOG("mmap img %s #%d: %s", n->name, i, strerror(errno));
				n->buf[i].img = NULL;
				return -1;
			}
		}
	}
	return 0;
}

static int qbuf(struct node *n, int index)
{
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	struct v4l2_buffer b;
	int p;

	memset(planes, 0, sizeof(planes));
	memset(&b, 0, sizeof(b));
	for (p = 0; p < n->nplanes; p++) {
		planes[p].m.fd = n->buf[index].fd[p];
		planes[p].length = n->buf[index].len[p];
	}
	b.type = n->type;
	b.memory = V4L2_MEMORY_DMABUF;
	b.index = index;
	b.m.planes = planes;
	b.length = n->nplanes;
	if (ioctl(n->fd, VIDIOC_QBUF, &b)) {
		LOG("QBUF %s #%d: %s", n->name, index, strerror(errno));
		return -1;
	}
	return 0;
}

/* returns the buffer index, or -1 */
static int dqbuf(struct node *n)
{
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	struct v4l2_buffer b;

	memset(planes, 0, sizeof(planes));
	memset(&b, 0, sizeof(b));
	b.type = n->type;
	b.memory = V4L2_MEMORY_DMABUF;
	b.m.planes = planes;
	b.length = n->nplanes;
	if (ioctl(n->fd, VIDIOC_DQBUF, &b)) {
		LOG("DQBUF %s: %s", n->name, strerror(errno));
		return -1;
	}
	return b.index;
}

static int streamon(struct node *n)
{
	int type = n->type;

	if (ioctl(n->fd, VIDIOC_STREAMON, &type)) {
		LOG("STREAMON %s: %s", n->name, strerror(errno));
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* shot metadata                                                       */
/* ------------------------------------------------------------------ */

static void fill_crop(uint32_t crop[4], int w, int h)
{
	crop[0] = 0;
	crop[1] = 0;
	crop[2] = w;
	crop[3] = h;
}

/*
 * One frame's shot for the head leader (3AA). Its node_group describes the
 * capture nodes of every group in the OTF chain: the 3AA and ISP junctions
 * and the MCSC output.
 */
static void fill_shot(struct node *leader, int index, uint32_t fcount)
{
	struct camera2_shot_ext *s = leader->buf[index].spare;

	memset(s, 0, SPARE_SIZE);

	s->shot.magicNumber = SHOT_MAGIC_NUMBER;
	s->shot.ctl.request.frameCount = fcount;

	s->shot.ctl.aa.mode = AA_CONTROL_AUTO;
	s->shot.ctl.aa.aeMode = AA_AEMODE_ON;
	s->shot.ctl.aa.awbMode = AA_AWBMODE_WB_AUTO;
	s->shot.ctl.aa.afMode = AA_AFMODE_OFF;
	s->shot.ctl.aa.effectMode = AA_EFFECT_OFF;
	s->shot.ctl.aa.sceneMode = AA_SCENE_MODE_DISABLED;
	s->shot.ctl.aa.captureIntent = AA_CAPTURE_INTENT_PREVIEW;
	s->shot.ctl.aa.aeTargetFpsRange[0] = OUT_FPS;
	s->shot.ctl.aa.aeTargetFpsRange[1] = OUT_FPS;
	s->shot.ctl.sensor.exposureTime = 16666666;
	s->shot.ctl.sensor.frameDuration = 1000000000 / OUT_FPS;
	s->shot.ctl.sensor.sensitivity = 100;
	fill_crop(s->shot.ctl.scaler.cropRegion, sen_w, sen_h);

	s->node_group.leader.vid = leader->vid;
	s->node_group.leader.request = 1;
	fill_crop(s->node_group.leader.input.cropRegion, sen_w, sen_h);
	fill_crop(s->node_group.leader.output.cropRegion, sen_w, sen_h);

	s->node_group.capture[0].vid = VID_30P;
	s->node_group.capture[0].request = 1;
	fill_crop(s->node_group.capture[0].input.cropRegion, sen_w, sen_h);
	fill_crop(s->node_group.capture[0].output.cropRegion, sen_w, sen_h);

	s->node_group.capture[1].vid = VID_I0P;
	s->node_group.capture[1].request = 1;
	fill_crop(s->node_group.capture[1].input.cropRegion, sen_w, sen_h);
	fill_crop(s->node_group.capture[1].output.cropRegion, sen_w, sen_h);

	s->node_group.capture[2].vid = VID_M0P;
	s->node_group.capture[2].request = 1;
	fill_crop(s->node_group.capture[2].input.cropRegion, sen_w, sen_h);
	fill_crop(s->node_group.capture[2].output.cropRegion, out_w, out_h);
}

/* ------------------------------------------------------------------ */
/* pipeline bring-up / tear-down                                       */
/* ------------------------------------------------------------------ */

static double now(void);

static void streamoff(struct node *n)
{
	int type = n->type;

	if (ioctl(n->fd, VIDIOC_STREAMOFF, &type))
		LOG("STREAMOFF %s: %s", n->name, strerror(errno));
}

static void cam_stop(void)
{
	size_t i;
	double t0 = now();

	if (stream_on) {
		/*
		 * Tell each group to cancel its pending shots when it stops. Without
		 * this the driver waits out its 3 s retry loops per stage for frames
		 * that can never complete once the sensor is off (~10 s to close).
		 */
		s_ctrl(&n_30s, CID_IS_FORCE_DONE, 1, "FORCE_DONE");
		s_ctrl(&n_i0s, CID_IS_FORCE_DONE, 1, "FORCE_DONE");
		s_ctrl(&n_m0s, CID_IS_FORCE_DONE, 1, "FORCE_DONE");
		s_ctrl(&n_ss0, CID_IS_S_STREAM, 0, "S_STREAM");
		stream_on = 0;
		/*
		 * Stop the head group (3AA) first: stopping it cancels the queued
		 * shots for the whole OTF chain. Stopping ISP/MCSC first makes each
		 * wait ~4.5 s for 3AA shots that only the 3AA stop can cancel.
		 */
		streamoff(&n_30s);
	}
	/*
	 * Closing everything is the tear-down path the driver has handled
	 * cleanly every time; the nodes go in reverse of how they were opened.
	 */
	for (i = NNODES; i-- > 0;)
		node_close(all_nodes[i]);
	LOG("camera stopped (%.1f s)", now() - t0);
}

static int cam_start(void)
{
	size_t i;
	int idx;

	LOG("camera starting");
	for (i = 0; i < NNODES; i++)
		if (node_open(all_nodes[i]))
			goto fail;

	if (s_input(&n_ss0, cam->sensor_id))
		goto fail;
	/* all ischain nodes are open: let the driver bring the chain up */
	if (s_ctrl(&n_30s, CID_IS_END_OF_STREAM, 0, "END_OF_STREAM"))
		goto fail;
	if (s_input(&n_30s, mk_input(cam->sensor_id, n_ss0.vid, 1, 1)) ||
	    s_input(&n_i0s, mk_input(cam->sensor_id, VID_30P, 1, 0)) ||
	    s_input(&n_m0s, mk_input(cam->sensor_id, VID_I0P, 1, 0)))
		goto fail;

	if (s_fmt(&n_ss0, V4L2_PIX_FMT_SBGGR10, sen_w, sen_h) ||
	    s_fmt(&n_30s, V4L2_PIX_FMT_SBGGR10, sen_w, sen_h) ||
	    s_fmt(&n_30p, V4L2_PIX_FMT_SBGGR10, sen_w, sen_h) ||
	    s_fmt(&n_i0s, V4L2_PIX_FMT_SBGGR10, sen_w, sen_h) ||
	    s_fmt(&n_i0p, V4L2_PIX_FMT_NV21, sen_w, sen_h) ||
	    s_fmt(&n_m0s, V4L2_PIX_FMT_NV21, sen_w, sen_h) ||
	    s_fmt(&n_m0p, V4L2_PIX_FMT_NV12, out_w, out_h))
		goto fail;

	{
		/* the sensor queue needs buffers allocated before STREAMON */
		struct v4l2_requestbuffers rb = {
			.count = 2, .type = n_ss0.type, .memory = V4L2_MEMORY_MMAP,
		};

		if (ioctl(n_ss0.fd, VIDIOC_REQBUFS, &rb)) {
			LOG("REQBUFS ss0: %s", strerror(errno));
			goto fail;
		}
	}
	if (reqbufs_dmabuf(&n_30s, NBUF, 0) || reqbufs_dmabuf(&n_i0s, NBUF, 0) ||
	    reqbufs_dmabuf(&n_m0s, NBUF, 0) || reqbufs_dmabuf(&n_m0p, NBUF, 1))
		goto fail;
	if (n_m0p.buf[0].img_len < out_size) {
		LOG("M0P image plane too small (%u < %d)", n_m0p.buf[0].img_len, out_size);
		goto fail;
	}

	for (idx = 0; idx < NBUF; idx++) {
		fill_shot(&n_30s, idx, idx + 1);
		if (qbuf(&n_m0p, idx))
			goto fail;
	}

	/* stream on: downstream first */
	if (streamon(&n_m0p) || streamon(&n_m0s) || streamon(&n_i0s) ||
	    streamon(&n_30s) || streamon(&n_ss0))
		goto fail;
	/* only the head group's leader (3AA) takes shots in the OTF chain */
	for (idx = 0; idx < NBUF; idx++)
		if (qbuf(&n_30s, idx))
			goto fail;
	if (s_ctrl(&n_ss0, CID_IS_S_STREAM, 1, "S_STREAM"))
		goto fail;
	stream_on = 1;
	LOG("camera streaming (%dx%d NV12)", out_w, out_h);
	return 0;

fail:
	cam_stop();
	return -1;
}

/* ------------------------------------------------------------------ */
/* rotation (the sensor sits sideways in the phone)                    */
/* ------------------------------------------------------------------ */

#define TILE 32

/*
 * Tiled rotation of a w x h plane of T-sized elements (T = 1 byte for Y,
 * 2 bytes for the interleaved UV pairs). One function per angle so the inner
 * loop is a plain strided copy.
 */
#define DEFINE_ROT(name, T, DI)							\
static void name(const T *src, T *dst, int w, int h)				\
{										\
	int x0, y0, x, y;							\
										\
	for (y0 = 0; y0 < h; y0 += TILE) {					\
		int ye = y0 + TILE < h ? y0 + TILE : h;				\
										\
		for (x0 = 0; x0 < w; x0 += TILE) {				\
			int xe = x0 + TILE < w ? x0 + TILE : w;			\
										\
			for (y = y0; y < ye; y++) {				\
				const T *sp = src + (size_t)y * w;		\
				for (x = x0; x < xe; x++)			\
					dst[DI] = sp[x];			\
			}							\
		}								\
	}									\
}

/* clockwise 90: (x,y) -> (h-1-y, x), destination is h wide */
DEFINE_ROT(rot90_8,   unsigned char,  (size_t)x * h + (h - 1 - y))
DEFINE_ROT(rot90_16,  unsigned short, (size_t)x * h + (h - 1 - y))
/* clockwise 270: (x,y) -> (y, w-1-x) */
DEFINE_ROT(rot270_8,  unsigned char,  (size_t)(w - 1 - x) * h + y)
DEFINE_ROT(rot270_16, unsigned short, (size_t)(w - 1 - x) * h + y)
/* 180: (x,y) -> (w-1-x, h-1-y), destination is w wide */
DEFINE_ROT(rot180_8,  unsigned char,  (size_t)(h - 1 - y) * w + (w - 1 - x))
DEFINE_ROT(rot180_16, unsigned short, (size_t)(h - 1 - y) * w + (w - 1 - x))

/* rotate an NV12 frame (Y plane + interleaved UV plane) clockwise by deg */
static void rotate_nv12(const unsigned char *src, unsigned char *dst, int deg)
{
	const unsigned char *suv = src + out_w * out_h;
	unsigned char *duv = dst + out_w * out_h;

	switch (deg) {
	case 90:
		rot90_8(src, dst, out_w, out_h);
		rot90_16((const unsigned short *)suv, (unsigned short *)duv,
			 out_w / 2, out_h / 2);
		break;
	case 270:
		rot270_8(src, dst, out_w, out_h);
		rot270_16((const unsigned short *)suv, (unsigned short *)duv,
			  out_w / 2, out_h / 2);
		break;
	default:
		rot180_8(src, dst, out_w, out_h);
		rot180_16((const unsigned short *)suv, (unsigned short *)duv,
			  out_w / 2, out_h / 2);
		break;
	}
}

/* ------------------------------------------------------------------ */
/* loopback output + reader detection                                  */
/* ------------------------------------------------------------------ */

static int loop_open(const char *path)
{
	struct v4l2_format f;
	struct v4l2_streamparm p;
	int fd = open(path, O_RDWR | O_CLOEXEC);

	if (fd < 0) {
		LOG("open %s: %s", path, strerror(errno));
		return -1;
	}
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	f.fmt.pix.width = pub_w;
	f.fmt.pix.height = pub_h;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	f.fmt.pix.field = V4L2_FIELD_NONE;
	f.fmt.pix.bytesperline = pub_w;
	f.fmt.pix.sizeimage = out_size;
	f.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	if (ioctl(fd, VIDIOC_S_FMT, &f)) {
		LOG("S_FMT on %s: %s", path, strerror(errno));
		close(fd);
		return -1;
	}
	memset(&p, 0, sizeof(p));
	p.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	p.parm.output.timeperframe.numerator = 1;
	p.parm.output.timeperframe.denominator = OUT_FPS;
	ioctl(fd, VIDIOC_S_PARM, &p);	/* advisory only */

	/*
	 * v4l2loopback only advertises capture formats once the producer has
	 * written a frame, so give idle readers (and apps probing the node)
	 * a black placeholder to find.
	 */
	{
		unsigned char *blank = malloc(out_size);

		if (blank) {
			memset(blank, 16, pub_w * pub_h);
			memset(blank + pub_w * pub_h, 128, pub_w * pub_h / 2);
			if (write(fd, blank, out_size) != out_size)
				LOG("placeholder write: %s", strerror(errno));
			free(blank);
		}
	}
	return fd;
}

/*
 * udev ran v4l_id when the loopback module was loaded, before we had written
 * a frame, so the node was tagged :video_output: only. Apps like Cheese list
 * cameras by ID_V4L_CAPABILITIES=:capture:, so ask udev to look again now.
 */
static void retrigger_udev(const char *path)
{
	const char *base = strrchr(path, '/');
	char match[64];
	pid_t pid;

	base = base ? base + 1 : path;
	snprintf(match, sizeof(match), "--sysname-match=%s", base);
	pid = fork();
	if (pid == 0) {
		execlp("udevadm", "udevadm", "trigger", "--action=change", match, (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
}

/* does any process other than us hold the loopback node open? */
static int has_reader(const char *path)
{
	DIR *proc = opendir("/proc");
	struct dirent *pe;
	pid_t self = getpid();
	char target[256];
	int found = 0;

	if (!proc)
		return 0;
	while (!found && (pe = readdir(proc))) {
		char dir[320];
		DIR *fds;
		struct dirent *fe;
		pid_t pid;

		if (pe->d_name[0] < '0' || pe->d_name[0] > '9')
			continue;
		pid = atoi(pe->d_name);
		if (pid == self)
			continue;
		snprintf(dir, sizeof(dir), "/proc/%s/fd", pe->d_name);
		fds = opendir(dir);
		if (!fds)
			continue;
		while ((fe = readdir(fds))) {
			char link[640];
			ssize_t n;

			if (fe->d_name[0] == '.')
				continue;
			snprintf(link, sizeof(link), "%s/%s", dir, fe->d_name);
			n = readlink(link, target, sizeof(target) - 1);
			if (n <= 0)
				continue;
			target[n] = 0;
			if (!strcmp(target, path)) {
				found = 1;
				break;
			}
		}
		closedir(fds);
	}
	closedir(proc);
	return found;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

/* stream frames until the readers go away; returns 0 to keep running */
static int run_stream(int loop_fd, const char *path)
{
	double last_reader = now(), last_scan = 0;
	unsigned long frames = 0;
	double t0 = now();

	while (!quit) {
		struct pollfd pfd = { .fd = n_m0p.fd, .events = POLLIN | POLLPRI };
		int pr, idx, sh;

		pr = poll(&pfd, 1, 1000);
		if (pr < 0 && errno == EINTR)
			continue;
		if (pr < 0) {
			LOG("poll: %s", strerror(errno));
			return -1;
		}
		if (pr == 0) {
			LOG("no frame for 1 s, restarting camera");
			return -1;
		}

		idx = dqbuf(&n_m0p);
		if (idx < 0)
			return -1;
		ion_sync_fd(n_m0p.buf[idx].fd[0]);
		if (rotation) {
			rotate_nv12(n_m0p.buf[idx].img, rot_buf, rotation);
			if (write(loop_fd, rot_buf, out_size) != out_size)
				DBG("loopback write: %s", strerror(errno));
		} else if (write(loop_fd, n_m0p.buf[idx].img, out_size) != out_size) {
			DBG("loopback write: %s", strerror(errno));
		}
		frames++;
		if (qbuf(&n_m0p, idx))
			return -1;

		/* the finished shot buffer comes back on the 3AA leader */
		sh = dqbuf(&n_30s);
		if (sh < 0)
			return -1;
		fill_shot(&n_30s, sh, NBUF + frames);
		if (qbuf(&n_30s, sh))
			return -1;

		if (now() - last_scan > SCAN_MS_RUN / 1000.0) {
			last_scan = now();
			if (has_reader(path))
				last_reader = now();
			else if (now() - last_reader > IDLE_STOP_SEC)
				break;
		}
	}
	LOG("streamed %lu frames in %.1f s", frames, now() - t0);
	return 0;
}

int main(int argc, char **argv)
{
	const char *path = "/dev/video20";
	int loop_fd, opt, lock_fd = -1;
	struct sigaction sa;

	int want_w = 480, want_h = 640;

	while ((opt = getopt(argc, argv, "c:d:r:s:v")) != -1) {
		switch (opt) {
		case 'd':
			path = optarg;
			break;
		case 'r':
			rotation = atoi(optarg);
			if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) {
				fprintf(stderr, "rotation must be 0, 90, 180 or 270\n");
				return 2;
			}
			break;
		case 'c': {
			size_t i;

			cam = NULL;
			for (i = 0; i < sizeof(cameras) / sizeof(cameras[0]); i++)
				if (!strcmp(optarg, cameras[i].name))
					cam = &cameras[i];
			if (!cam) {
				fprintf(stderr, "-c expects rear or front\n");
				return 2;
			}
			break;
		}
		case 's':
			if (sscanf(optarg, "%dx%d", &want_w, &want_h) != 2 ||
			    want_w < 64 || want_h < 64 || want_w > 1920 || want_h > 1920) {
				fprintf(stderr, "-s expects WxH, e.g. 480x640\n");
				return 2;
			}
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			fprintf(stderr, "usage: %s [-c rear|front] [-d /dev/videoN] [-r 0|90|180|270] [-s WxH] [-v]\n", argv[0]);
			return 2;
		}
	}

	if (getenv("JOURNAL_STREAM"))
		log_prefix = "<5>camd: ";

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	/* apply the camera profile: its sensor node, mode and default rotation */
	sen_w = cam->sen_w;
	sen_h = cam->sen_h;
	n_ss0.vid = cam->ss_vid;
	n_ss0.sysname = cam->ss_sysname;
	if (rotation < 0)
		rotation = cam->rotation;

	/* the scaler works in sensor orientation: swap for 90/270 */
	pub_w = want_w;
	pub_h = want_h;
	if (rotation == 90 || rotation == 270) {
		out_w = want_h;
		out_h = want_w;
	} else {
		out_w = want_w;
		out_h = want_h;
	}
	/* MCSC strides are 16-byte aligned; keep frames tightly packed */
	if ((out_w % 16) || (out_h % 2)) {
		fprintf(stderr, "scaler width must be a multiple of 16 and height even (got %dx%d)\n",
			out_w, out_h);
		return 2;
	}
	out_size = out_w * out_h * 3 / 2;
	rot_buf = malloc(out_size);
	if (!rot_buf) {
		LOG("out of memory");
		return 1;
	}

	loop_fd = loop_open(path);
	if (loop_fd < 0)
		return 1;
	retrigger_udev(path);
	LOG("%s camera: publishing %dx%d NV12 (rotated %d) on %s; waiting for a reader", cam->name, pub_w, pub_h, rotation, path);

	while (!quit) {
		if (!has_reader(path)) {
			struct timespec ts = { 0, SCAN_MS_IDLE * 1000000L };

			nanosleep(&ts, NULL);
			continue;
		}
		LOG("reader present");
		/*
		 * Both cameras share one 3AA/ISP/MCSC chain, and a failed bring-up
		 * can leave the driver wedged, so only one camd at a time may start
		 * it. Wait for the other camera's reader to go away.
		 */
		if (lock_fd < 0)
			lock_fd = open("/run/camd-pipeline.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
		if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB)) {
			struct timespec ts = { 0, SCAN_MS_IDLE * 1000000L };

			DBG("camera pipeline busy (other camera streaming)");
			nanosleep(&ts, NULL);
			continue;
		}
		if (cam_start()) {
			if (lock_fd >= 0)
				flock(lock_fd, LOCK_UN);
			/* do not hammer the driver if bring-up keeps failing */
			sleep(5);
			continue;
		}
		if (run_stream(loop_fd, path) < 0)
			sleep(2);
		cam_stop();
		if (lock_fd >= 0)
			flock(lock_fd, LOCK_UN);
	}
	close(loop_fd);
	return 0;
}
