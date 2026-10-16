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
 * Run:                 camd [-d /dev/video20] [-v]
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
#include <sys/mman.h>
#include <linux/videodev2.h>
#define _LINUX_TYPES_H /* header typedefs clash with libc; stdint is already in */
#include "fimc-is-metadata.h"
#include "ion.h"

#define CID_BASE		(V4L2_CTRL_CLASS_CAMERA | 0x1000)
#define CID_IS_S_STREAM		(CID_BASE + 14)
#define CID_IS_END_OF_STREAM	(CID_BASE + 58)

/* video ids (device node = /dev/video(100 + id)) */
#define VID_SS0		1
#define VID_30S		10
#define VID_30P		12
#define VID_I0S		30
#define VID_I0P		32
#define VID_M0S		60
#define VID_M0P		70

#define SENSOR_3L2	8

/* 3L2 mode 3: 2072x1552 @30 fps, scaled to a 4:3 NV12 output */
#define SEN_W		2072
#define SEN_H		1552
#define OUT_W		1280
#define OUT_H		960
#define OUT_FPS		30
#define OUT_SIZE	(OUT_W * OUT_H * 3 / 2)

#define SPARE_SIZE	(32 * 1024)
#define MAXBUF		4
#define NBUF		4

#define IDLE_STOP_SEC	5
#define SCAN_MS_IDLE	500
#define SCAN_MS_RUN	2000

static int verbose;
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
	int vid;
	int fd;
	enum v4l2_buf_type type;
	int nbufs;
	int nplanes;
	struct buf buf[MAXBUF];
};

static struct node n_ss0 = { "ss0",  VID_SS0, -1, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
static struct node n_30s = { "3aa",  VID_30S, -1, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
static struct node n_30p = { "3aap", VID_30P, -1, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
static struct node n_i0s = { "isp",  VID_I0S, -1, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
static struct node n_i0p = { "ispp", VID_I0P, -1, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
static struct node n_m0s = { "mcs",  VID_M0S, -1, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
static struct node n_m0p = { "mcsp", VID_M0P, -1, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };

static struct node *all_nodes[] = {
	&n_ss0, &n_30s, &n_30p, &n_i0s, &n_i0p, &n_m0s, &n_m0p,
};
#define NNODES (sizeof(all_nodes) / sizeof(all_nodes[0]))

static int ion_fd = -1;
static int stream_on;		/* sensor front-start issued */

/* ------------------------------------------------------------------ */
/* ION buffers                                                         */
/* ------------------------------------------------------------------ */

/* uncached system-heap ION buffer -> dma-buf fd */
static int ion_alloc_fd(size_t len)
{
	struct ion_allocation_data a = {
		.len = len, .align = 4096, .heap_id_mask = 1 /* system */, .flags = 0,
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

static int node_open(struct node *n)
{
	char path[32];

	snprintf(path, sizeof(path), "/dev/video%d", 100 + n->vid);
	n->fd = open(path, O_RDWR | O_CLOEXEC);
	if (n->fd < 0) {
		LOG("open %s (%s): %s", path, n->name, strerror(errno));
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
			n->buf[i].fd[p] = ion_alloc_fd(planes[p].length);
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
	fill_crop(s->shot.ctl.scaler.cropRegion, SEN_W, SEN_H);

	s->node_group.leader.vid = leader->vid;
	s->node_group.leader.request = 1;
	fill_crop(s->node_group.leader.input.cropRegion, SEN_W, SEN_H);
	fill_crop(s->node_group.leader.output.cropRegion, SEN_W, SEN_H);

	s->node_group.capture[0].vid = VID_30P;
	s->node_group.capture[0].request = 1;
	fill_crop(s->node_group.capture[0].input.cropRegion, SEN_W, SEN_H);
	fill_crop(s->node_group.capture[0].output.cropRegion, SEN_W, SEN_H);

	s->node_group.capture[1].vid = VID_I0P;
	s->node_group.capture[1].request = 1;
	fill_crop(s->node_group.capture[1].input.cropRegion, SEN_W, SEN_H);
	fill_crop(s->node_group.capture[1].output.cropRegion, SEN_W, SEN_H);

	s->node_group.capture[2].vid = VID_M0P;
	s->node_group.capture[2].request = 1;
	fill_crop(s->node_group.capture[2].input.cropRegion, SEN_W, SEN_H);
	fill_crop(s->node_group.capture[2].output.cropRegion, OUT_W, OUT_H);
}

/* ------------------------------------------------------------------ */
/* pipeline bring-up / tear-down                                       */
/* ------------------------------------------------------------------ */

static void cam_stop(void)
{
	size_t i;

	if (stream_on) {
		s_ctrl(&n_ss0, CID_IS_S_STREAM, 0, "S_STREAM");
		stream_on = 0;
	}
	/*
	 * Closing everything is the tear-down path the driver has handled
	 * cleanly every time; the nodes go in reverse of how they were opened.
	 */
	for (i = NNODES; i-- > 0;)
		node_close(all_nodes[i]);
	LOG("camera stopped");
}

static int cam_start(void)
{
	size_t i;
	int idx;

	LOG("camera starting");
	for (i = 0; i < NNODES; i++)
		if (node_open(all_nodes[i]))
			goto fail;

	if (s_input(&n_ss0, SENSOR_3L2))
		goto fail;
	/* all ischain nodes are open: let the driver bring the chain up */
	if (s_ctrl(&n_30s, CID_IS_END_OF_STREAM, 0, "END_OF_STREAM"))
		goto fail;
	if (s_input(&n_30s, mk_input(SENSOR_3L2, VID_SS0, 1, 1)) ||
	    s_input(&n_i0s, mk_input(SENSOR_3L2, VID_30P, 1, 0)) ||
	    s_input(&n_m0s, mk_input(SENSOR_3L2, VID_I0P, 1, 0)))
		goto fail;

	if (s_fmt(&n_ss0, V4L2_PIX_FMT_SBGGR10, SEN_W, SEN_H) ||
	    s_fmt(&n_30s, V4L2_PIX_FMT_SBGGR10, SEN_W, SEN_H) ||
	    s_fmt(&n_30p, V4L2_PIX_FMT_SBGGR10, SEN_W, SEN_H) ||
	    s_fmt(&n_i0s, V4L2_PIX_FMT_SBGGR10, SEN_W, SEN_H) ||
	    s_fmt(&n_i0p, V4L2_PIX_FMT_NV21, SEN_W, SEN_H) ||
	    s_fmt(&n_m0s, V4L2_PIX_FMT_NV21, SEN_W, SEN_H) ||
	    s_fmt(&n_m0p, V4L2_PIX_FMT_NV12, OUT_W, OUT_H))
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
	if (n_m0p.buf[0].img_len < OUT_SIZE) {
		LOG("M0P image plane too small (%u < %d)", n_m0p.buf[0].img_len, OUT_SIZE);
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
	LOG("camera streaming (%dx%d NV12)", OUT_W, OUT_H);
	return 0;

fail:
	cam_stop();
	return -1;
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
	f.fmt.pix.width = OUT_W;
	f.fmt.pix.height = OUT_H;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	f.fmt.pix.field = V4L2_FIELD_NONE;
	f.fmt.pix.bytesperline = OUT_W;
	f.fmt.pix.sizeimage = OUT_SIZE;
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
		unsigned char *blank = malloc(OUT_SIZE);

		if (blank) {
			memset(blank, 16, OUT_W * OUT_H);
			memset(blank + OUT_W * OUT_H, 128, OUT_W * OUT_H / 2);
			if (write(fd, blank, OUT_SIZE) != OUT_SIZE)
				LOG("placeholder write: %s", strerror(errno));
			free(blank);
		}
	}
	return fd;
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
		if (write(loop_fd, n_m0p.buf[idx].img, OUT_SIZE) != OUT_SIZE)
			DBG("loopback write: %s", strerror(errno));
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
	int loop_fd, opt;
	struct sigaction sa;

	while ((opt = getopt(argc, argv, "d:v")) != -1) {
		switch (opt) {
		case 'd':
			path = optarg;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			fprintf(stderr, "usage: %s [-d /dev/videoN] [-v]\n", argv[0]);
			return 2;
		}
	}

	if (getenv("JOURNAL_STREAM"))
		log_prefix = "<5>camd: ";

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	loop_fd = loop_open(path);
	if (loop_fd < 0)
		return 1;
	LOG("publishing %dx%d NV12 on %s; waiting for a reader", OUT_W, OUT_H, path);

	while (!quit) {
		if (!has_reader(path)) {
			struct timespec ts = { 0, SCAN_MS_IDLE * 1000000L };

			nanosleep(&ts, NULL);
			continue;
		}
		LOG("reader present");
		if (cam_start()) {
			/* do not hammer the driver if bring-up keeps failing */
			sleep(5);
			continue;
		}
		if (run_stream(loop_fd, path) < 0)
			sleep(2);
		cam_stop();
	}
	close(loop_fd);
	return 0;
}
