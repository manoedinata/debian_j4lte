/*
 * camtest - staged bring-up of the J4 rear camera (S5K3L2) through the
 * FIMC-IS2 OTF pipeline:  SS0 -> 3AA0 -> ISP0 -> MCSC0 (-> M0P)
 *
 * usage: camtest <stage> [frames]
 *   1  open nodes, END_OF_STREAM, S_INPUT (sensor, 3AA, ISP, MCSC)
 *   2  + S_FMT on every node
 *   3  + REQBUFS, QUERYBUF, mmap, QBUF the first shots
 *   4  + STREAMON (downstream first), sensor front start
 *   5  + DQBUF frames from MCSC out and dump the first as /tmp/frame.nv21
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#define _LINUX_TYPES_H /* header typedefs clash with libc; stdint is already in */
#include "fimc-is-metadata.h"
#include "ion.h"

#define CID_BASE		(V4L2_CTRL_CLASS_CAMERA | 0x1000)
#define CID_IS_S_STREAM		(CID_BASE + 14)
#define CID_IS_S_SENSOR_SIZE	(CID_BASE + 69)
#define CID_IS_END_OF_STREAM	(CID_BASE + 58)
#define CID_IS_CAMERA_TYPE	(CID_BASE + 61)
#define CID_IS_HAL_VERSION	(CID_BASE + 902)

/* video ids (device node = /dev/video(100 + id)) */
#define VID_SS0		1
#define VID_30S		10
#define VID_30C		11
#define VID_30P		12
#define VID_I0S		30
#define VID_I0C		31
#define VID_I0P		32
#define VID_M0S		60
#define VID_M0P		70

#define SENSOR_3L2	8

/* 3L2 mode 3: 2072x1552 @30 fps */
#define SEN_W		2072
#define SEN_H		1552
#define OUT_W		1280
#define OUT_H		960

#define SPARE_SIZE	(32 * 1024)
#define MAXBUF		4
#define NBUF		4

struct buf {
	int fd[VIDEO_MAX_PLANES];		/* ION dma-buf fds, one per plane */
	unsigned int len[VIDEO_MAX_PLANES];
	void *spare;		/* mapped spare plane (shot_ext) */
	void *img;		/* mapped image plane (M0P only) */
	unsigned int img_len;
};

static int ion_fd = -1;

/* allocate an uncached system-heap ION buffer and return a dma-buf fd */
static int ion_alloc_fd(size_t len)
{
	struct ion_allocation_data a = {
		.len = len, .align = 4096, .heap_id_mask = 1 /* system */, .flags = 0,
	};
	struct ion_fd_data d;
	struct ion_handle_data h;

	if (ion_fd < 0) {
		ion_fd = open("/dev/ion", O_RDWR);
		if (ion_fd < 0) {
			perror("open /dev/ion");
			return -1;
		}
	}
	if (ioctl(ion_fd, ION_IOC_ALLOC, &a)) {
		perror("ION_IOC_ALLOC");
		return -1;
	}
	d.handle = a.handle;
	d.fd = -1;
	if (ioctl(ion_fd, ION_IOC_SHARE, &d)) {
		perror("ION_IOC_SHARE");
		return -1;
	}
	h.handle = a.handle;
	ioctl(ion_fd, ION_IOC_FREE, &h);	/* the fd keeps the buffer alive */
	return d.fd;
}

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

static int xopen(struct node *n)
{
	char path[32];

	snprintf(path, sizeof(path), "/dev/video%d", 100 + n->vid);
	n->fd = open(path, O_RDWR);
	if (n->fd < 0) {
		fprintf(stderr, "open %s (%s): %s\n", path, n->name, strerror(errno));
		return -1;
	}
	printf("open    %-5s %s fd=%d\n", n->name, path, n->fd);
	return 0;
}

#define CHK(call, what, n) ({ \
	int _r = (call); \
	printf("%-8s %-5s -> %d %s\n", what, (n)->name, _r, _r ? strerror(errno) : ""); \
	_r; })

static int s_ctrl(struct node *n, unsigned int id, int value, const char *what)
{
	struct v4l2_control c = { .id = id, .value = value };
	int ret = ioctl(n->fd, VIDIOC_S_CTRL, &c);

	printf("S_CTRL   %-5s %-14s=%-4d -> %d %s\n", n->name, what, value, ret,
	       ret ? strerror(errno) : "");
	return ret;
}

static int s_input(struct node *n, unsigned int input)
{
	int ret = ioctl(n->fd, VIDIOC_S_INPUT, &input);

	printf("S_INPUT  %-5s 0x%08x -> %d %s\n", n->name, input, ret,
	       ret ? strerror(errno) : "");
	return ret;
}

/* S_INPUT layout: stream[25:24] module[23:16] vindex[15:8] intype[7:4] leader[3:0] */
static unsigned int mk_input(int module, int vindex, int intype, int leader)
{
	return (module << 16) | (vindex << 8) | (intype << 4) | leader;
}

static int s_fmt(struct node *n, unsigned int pixfmt, int w, int h)
{
	struct v4l2_format f;
	int ret;

	memset(&f, 0, sizeof(f));
	f.type = n->type;
	f.fmt.pix_mp.width = w;
	f.fmt.pix_mp.height = h;
	f.fmt.pix_mp.pixelformat = pixfmt;
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	f.fmt.pix_mp.num_planes = 1;
	ret = ioctl(n->fd, VIDIOC_S_FMT, &f);
	printf("S_FMT    %-5s %c%c%c%c %dx%d -> %d %s\n", n->name,
	       pixfmt & 0xff, (pixfmt >> 8) & 0xff, (pixfmt >> 16) & 0xff,
	       pixfmt >> 24, w, h, ret, ret ? strerror(errno) : "");
	return ret;
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
		printf("REQBUFS  %-5s %d -> %s\n", n->name, count, strerror(errno));
		return -1;
	}
	printf("REQBUFS  %-5s %d -> got %d\n", n->name, count, rb.count);
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
			printf("QUERYBUF %-5s #%d -> %s\n", n->name, i, strerror(errno));
			return -1;
		}
		n->nplanes = b.length;
		if (i == 0) {
			printf("QUERYBUF %-5s planes=%d:", n->name, b.length);
			for (p = 0; p < (int)b.length; p++)
				printf(" [%u]", planes[p].length);
			printf("\n");
		}
		for (p = 0; p < (int)b.length; p++) {
			n->buf[i].len[p] = planes[p].length;
			n->buf[i].fd[p] = ion_alloc_fd(planes[p].length);
			if (n->buf[i].fd[p] < 0)
				return -1;
		}
		/* spare plane is the last one; the shot metadata lives there */
		p = b.length - 1;
		n->buf[i].spare = mmap(NULL, n->buf[i].len[p], PROT_READ | PROT_WRITE,
				       MAP_SHARED, n->buf[i].fd[p], 0);
		if (n->buf[i].spare == MAP_FAILED) {
			printf("mmap spare %-5s #%d -> %s\n", n->name, i, strerror(errno));
			n->buf[i].spare = NULL;
			return -1;
		}
		memset(n->buf[i].spare, 0, n->buf[i].len[p]);
		if (map_img) {
			n->buf[i].img_len = n->buf[i].len[0];
			n->buf[i].img = mmap(NULL, n->buf[i].len[0], PROT_READ | PROT_WRITE,
					     MAP_SHARED, n->buf[i].fd[0], 0);
			if (n->buf[i].img == MAP_FAILED) {
				printf("mmap img %-5s #%d -> %s\n", n->name, i, strerror(errno));
				n->buf[i].img = NULL;
			}
		}
	}
	return 0;
}

static int qbuf(struct node *n, int index)
{
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	struct v4l2_buffer b;
	int ret;

	memset(planes, 0, sizeof(planes));
	memset(&b, 0, sizeof(b));
	{
		int p;

		for (p = 0; p < n->nplanes; p++) {
			planes[p].m.fd = n->buf[index].fd[p];
			planes[p].length = n->buf[index].len[p];
		}
	}
	b.type = n->type;
	b.memory = V4L2_MEMORY_DMABUF;
	b.index = index;
	b.m.planes = planes;
	b.length = n->nplanes;
	ret = ioctl(n->fd, VIDIOC_QBUF, &b);
	if (ret)
		printf("QBUF     %-5s #%d -> %s\n", n->name, index, strerror(errno));
	return ret;
}

static void fill_crop(uint32_t crop[4], int w, int h)
{
	crop[0] = 0;
	crop[1] = 0;
	crop[2] = w;
	crop[3] = h;
}

/*
 * Fill the shot metadata for one frame. Only the head group's leader (3AA)
 * takes shots in an OTF chain; its node_group lists the capture nodes of
 * every group in the chain (3AA junction, ISP junction, MCSC output).
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
	s->shot.ctl.aa.aeTargetFpsRange[0] = 30;
	s->shot.ctl.aa.aeTargetFpsRange[1] = 30;
	s->shot.ctl.sensor.exposureTime = 16666666;
	s->shot.ctl.sensor.frameDuration = 33333333;
	s->shot.ctl.sensor.sensitivity = 100;
	fill_crop(s->shot.ctl.scaler.cropRegion, SEN_W, SEN_H);

	s->setfile = 0;

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

static int streamon(struct node *n)
{
	int type = n->type;
	int ret = ioctl(n->fd, VIDIOC_STREAMON, &type);

	printf("STREAMON %-5s -> %d %s\n", n->name, ret, ret ? strerror(errno) : "");
	return ret;
}

int main(int argc, char **argv)
{
	int stage = argc > 1 ? atoi(argv[1]) : 1;
	int frames = argc > 2 ? atoi(argv[2]) : 3;
	int i;

	setvbuf(stdout, NULL, _IOLBF, 0);

	if (xopen(&n_ss0) || xopen(&n_30s) || xopen(&n_30p) ||
	    xopen(&n_i0s) || xopen(&n_i0p) || xopen(&n_m0s) || xopen(&n_m0p))
		return 1;

	/* sensor (scenario 0 = normal) */
	if (s_input(&n_ss0, SENSOR_3L2))
		return 1;
	/* all ischain nodes are open: let the driver bring the chain up */
	if (s_ctrl(&n_30s, CID_IS_END_OF_STREAM, 0, "END_OF_STREAM"))
		return 1;
	/* 3AA: leader, OTF from the sensor video node */
	if (s_input(&n_30s, mk_input(SENSOR_3L2, VID_SS0, 1, 1)))
		return 1;
	/* ISP: OTF from the 3AA capture node junction (3AP) */
	if (s_input(&n_i0s, mk_input(SENSOR_3L2, VID_30P, 1, 0)))
		return 1;
	/* MCSC: OTF from the ISP capture node junction (ISPP) */
	if (s_input(&n_m0s, mk_input(SENSOR_3L2, VID_I0P, 1, 0)))
		return 1;
	if (stage < 2)
		goto done;

	/* --- formats --- */
	if (s_fmt(&n_ss0, V4L2_PIX_FMT_SBGGR10, SEN_W, SEN_H) ||
	    s_fmt(&n_30s, V4L2_PIX_FMT_SBGGR10, SEN_W, SEN_H) ||
	    s_fmt(&n_30p, V4L2_PIX_FMT_SBGGR10, SEN_W, SEN_H) ||
	    s_fmt(&n_i0s, V4L2_PIX_FMT_SBGGR10, SEN_W, SEN_H) ||
	    s_fmt(&n_i0p, V4L2_PIX_FMT_NV21, SEN_W, SEN_H) ||
	    s_fmt(&n_m0s, V4L2_PIX_FMT_NV21, SEN_W, SEN_H) ||
	    s_fmt(&n_m0p, V4L2_PIX_FMT_NV21, OUT_W, OUT_H))
		return 1;
	if (stage < 3)
		goto done;

	/* --- buffers --- */
	if (CHK(ioctl(n_ss0.fd, VIDIOC_REQBUFS, &(struct v4l2_requestbuffers){
		.count = 2, .type = n_ss0.type, .memory = V4L2_MEMORY_MMAP }),
		"REQBUFS2", &n_ss0))
		return 1;
	if (reqbufs_dmabuf(&n_30s, NBUF, 0) || reqbufs_dmabuf(&n_i0s, NBUF, 0) ||
	    reqbufs_dmabuf(&n_m0s, NBUF, 0) || reqbufs_dmabuf(&n_m0p, NBUF, 1))
		return 1;

	for (i = 0; i < NBUF; i++) {
		/* one shot per pipe per frame, same frame count everywhere */
		fill_shot(&n_30s, i, i + 1);
		if (qbuf(&n_m0p, i))
			return 1;
	}
	if (stage < 4)
		goto done;

	/* --- stream on: downstream first --- */
	if (streamon(&n_m0p) || streamon(&n_m0s) || streamon(&n_i0s) ||
	    streamon(&n_30s) || streamon(&n_ss0))
		return 1;
	/* only the head group's leader (3AA) takes shots in the OTF chain */
	for (i = 0; i < NBUF; i++)
		if (qbuf(&n_30s, i))
			return 1;
	if (s_ctrl(&n_ss0, CID_IS_S_STREAM, 1, "S_STREAM"))
		return 1;
	if (stage < 5)
		goto done;

	/* --- frames: recycle capture + shot buffers so AE can converge --- */
	for (i = 0; i < frames; i++) {
		struct v4l2_plane planes[VIDEO_MAX_PLANES];
		struct v4l2_buffer b;
		struct pollfd pfd = { .fd = n_m0p.fd, .events = POLLIN | POLLPRI };
		int pr = poll(&pfd, 1, 5000);
		int idx;

		if (pr <= 0) {
			printf("poll m0p -> %d (revents %#x)\n", pr, pfd.revents);
			break;
		}
		memset(planes, 0, sizeof(planes));
		memset(&b, 0, sizeof(b));
		b.type = n_m0p.type;
		b.memory = V4L2_MEMORY_DMABUF;
		b.m.planes = planes;
		b.length = n_m0p.nplanes;
		if (ioctl(n_m0p.fd, VIDIOC_DQBUF, &b)) {
			printf("DQBUF m0p -> %s\n", strerror(errno));
			break;
		}
		idx = b.index;
		if (i < 3 || i % 10 == 0 || i == frames - 1) {
			unsigned long sum = 0;
			unsigned char *y = n_m0p.buf[idx].img;
			int k;

			for (k = 0; y && k < OUT_W * OUT_H; k += 97)
				sum += y[k];
			printf("frame %3d: DQBUF m0p #%d, mean Y ~ %lu\n", i, idx,
			       sum / (OUT_W * OUT_H / 97));
		}
		if (i == frames - 1 && n_m0p.buf[idx].img) {
			FILE *f = fopen("/tmp/frame.nv21", "wb");

			if (f) {
				fwrite(n_m0p.buf[idx].img, 1, OUT_W * OUT_H * 3 / 2, f);
				fclose(f);
				printf("wrote /tmp/frame.nv21 (%dx%d NV21)\n", OUT_W, OUT_H);
			}
		}

		/* return the capture buffer, and recycle a finished shot buffer */
		if (qbuf(&n_m0p, idx))
			break;
		if (i >= 0) {
			struct v4l2_plane sp[VIDEO_MAX_PLANES];
			struct v4l2_buffer sb;

			memset(sp, 0, sizeof(sp));
			memset(&sb, 0, sizeof(sb));
			sb.type = n_30s.type;
			sb.memory = V4L2_MEMORY_DMABUF;
			sb.m.planes = sp;
			sb.length = n_30s.nplanes;
			if (ioctl(n_30s.fd, VIDIOC_DQBUF, &sb)) {
				printf("DQBUF 3aa -> %s\n", strerror(errno));
				break;
			}
			if (i < 3 || i % 10 == 0 || i == frames - 1) {
				struct camera2_shot_ext *d = n_30s.buf[sb.index].spare;

				printf("          dm: frame %u exp %llu ns iso %u aeState %d fps %u-%u\n",
				       d->shot.dm.request.frameCount,
				       (unsigned long long)d->shot.dm.sensor.exposureTime,
				       d->shot.dm.sensor.sensitivity, d->shot.dm.aa.aeState,
				       d->shot.dm.aa.aeTargetFpsRange[0],
				       d->shot.dm.aa.aeTargetFpsRange[1]);
			}
			fill_shot(&n_30s, sb.index, NBUF + i + 1);
			if (qbuf(&n_30s, sb.index))
				break;
		}
	}

	s_ctrl(&n_ss0, CID_IS_S_STREAM, 0, "S_STREAM");
done:
	printf("stage %d reached; holding 3 s\n", stage);
	sleep(3);
	return 0;
}
