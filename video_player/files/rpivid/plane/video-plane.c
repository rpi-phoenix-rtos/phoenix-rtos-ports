/*
 * Phoenix-RTOS
 *
 * video-plane: HEVC on the rpivid block, shown on a display overlay with no CPU pixel work
 * (docs/misc/2026-11-21-yuv-sand-plane-design.md, step 2).
 *
 * hevc_rpivid decodes with rpivid_out=drm_prime into picture buffers that come from rpi4-kms
 * itself (dumb buffers: its scan-out pool, below 1 GiB, which is all the firmware plane can
 * fetch). Every picture is one NV12 buffer in SAND128 columns; it is added as a framebuffer
 * once (ADDFB2, DRM_FORMAT_MOD_BROADCOM_SAND128) and put on the overlay at its presentation
 * time. The CPU never touches a pixel. A picture stays referenced until two later ones have
 * been shown, so the decoder never reuses a buffer the plane may still be scanning.
 *
 *   video-plane [-n frames] [-x] file.mp4|.mkv|.hevc
 *     -n  stop after that many pictures      -x  as fast as decoded (no pacing)
 *
 * Lines start with "VIDEOPLANE "; the last is
 *   VIDEOPLANE result=PASS|FAIL decoded=N shown=N wall_s=W cpu_s=C cpu_pct=P fps=F
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/rpivid_drm.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/time.h>

#define PAGE     4096u
#define MAX_FBS  48
#define HOLD     3   /* pictures kept referenced: the one shown, and the two before it */

static int card = -1;

typedef struct {
	uint32_t handle;
	uint32_t fb;
} fb_slot_t;

static fb_slot_t fbs[MAX_FBS];
static int nfbs;


/* ---- the decoder's picture buffers: rpi4-kms dumb buffers ---- */

static int kms_alloc(void *opaque, size_t size, RpividDrmBuffer *b)
{
	struct drm_mode_create_dumb cd;
	struct drm_mode_map_dumb md;
	struct drm_mode_destroy_dumb dd;
	void *p;
	size_t i;
	int fd = -1;

	(void)opaque;
	size = (size + PAGE - 1u) & ~(size_t)(PAGE - 1u);
	memset(&cd, 0, sizeof(cd));
	cd.width = PAGE;                       /* 8-bit "pixels", one page per row */
	cd.height = (uint32_t)(size / PAGE);
	cd.bpp = 8u;
	if (drmIoctl(card, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0) {
		return -errno;
	}
	memset(&md, 0, sizeof(md));
	md.handle = cd.handle;
	p = MAP_FAILED;
	if (drmIoctl(card, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0) {
		p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, card, (off_t)md.offset);
	}
	if (p == MAP_FAILED) {
		memset(&dd, 0, sizeof(dd));
		dd.handle = cd.handle;
		(void)drmIoctl(card, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
		return -ENOMEM;
	}
	/* the block has no MMU: one physically contiguous range (the pool's buffers are) */
	b->pa = (uint64_t)va2pa(p);
	for (i = PAGE; (b->pa != 0u) && (i < size); i += PAGE) {
		if ((uint64_t)va2pa((uint8_t *)p + i) != b->pa + i) {
			b->pa = 0;
		}
	}
	if ((b->pa == 0u) || (drmPrimeHandleToFD(card, cd.handle, DRM_CLOEXEC, &fd) != 0)) {
		munmap(p, size);
		memset(&dd, 0, sizeof(dd));
		dd.handle = cd.handle;
		(void)drmIoctl(card, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
		return (b->pa == 0u) ? -EFAULT : -errno;
	}
	b->cpu = p;
	b->size = size;
	b->fd = fd;
	b->handle = cd.handle;
	b->priv = NULL;
	return 0;
}


static void kms_free(void *opaque, RpividDrmBuffer *b)
{
	struct drm_mode_destroy_dumb dd;
	int i;

	(void)opaque;
	for (i = 0; i < nfbs; i++) {
		if (fbs[i].handle == b->handle) {
			(void)drmModeRmFB(card, fbs[i].fb);
			fbs[i] = fbs[--nfbs];
			break;
		}
	}
	if (b->cpu != NULL) {
		munmap(b->cpu, b->size);
	}
	if (b->fd >= 0) {
		close(b->fd);
	}
	memset(&dd, 0, sizeof(dd));
	dd.handle = b->handle;
	(void)drmIoctl(card, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
	memset(b, 0, sizeof(*b));
	b->fd = -1;
}


static int kms_wait_idle(void *opaque, const RpividDrmBuffer *b)
{
	(void)opaque;
	(void)b;
	return 0;   /* no GPU reads them; the plane is covered by HOLD */
}


/* ---- display ---- */

static int find_plane(uint32_t *plane_id)
{
	drmModePlaneRes *pr = drmModeGetPlaneResources(card);
	uint32_t i, j;
	int found = 0;

	for (i = 0; (pr != NULL) && (i < pr->count_planes) && !found; i++) {
		drmModePlane *p = drmModeGetPlane(card, pr->planes[i]);
		for (j = 0; (p != NULL) && (j < p->count_formats); j++) {
			if ((p->formats[j] == DRM_FORMAT_NV12) && ((p->possible_crtcs & 1u) != 0u)) {
				*plane_id = p->plane_id;
				found = 1;
				break;
			}
		}
		drmModeFreePlane(p);
	}
	drmModeFreePlaneResources(pr);
	return found;
}


/* The framebuffer of a decoded picture (added once per buffer) */
static int picture_fb(const AVFrame *f, uint32_t *fb)
{
	const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *)f->data[0];
	uint32_t h, handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	uint64_t mods[4] = { 0 };
	int i, rc;

	if ((d == NULL) || (d->nb_objects != 1) || (d->nb_layers != 1) || (d->layers[0].nb_planes != 2)) {
		return -EINVAL;
	}
	if (drmPrimeFDToHandle(card, d->objects[0].fd, &h) != 0) {
		return -errno;
	}
	for (i = 0; i < nfbs; i++) {
		if (fbs[i].handle == h) {
			*fb = fbs[i].fb;
			return 0;
		}
	}
	if (nfbs >= MAX_FBS) {
		return -ENOSPC;
	}
	handles[0] = handles[1] = h;
	pitches[0] = (uint32_t)d->layers[0].planes[0].pitch;
	pitches[1] = (uint32_t)d->layers[0].planes[1].pitch;
	offsets[0] = (uint32_t)d->layers[0].planes[0].offset;
	offsets[1] = (uint32_t)d->layers[0].planes[1].offset;
	mods[0] = mods[1] = d->objects[0].format_modifier;
	rc = drmModeAddFB2WithModifiers(card, (uint32_t)f->width, (uint32_t)f->height, DRM_FORMAT_NV12, handles, pitches,
		offsets, mods, fb, DRM_MODE_FB_MODIFIERS);
	if (rc != 0) {
		return rc;
	}
	fbs[nfbs].handle = h;
	fbs[nfbs].fb = *fb;
	nfbs++;
	return 0;
}


static double cpu_seconds(void)
{
	struct timespec t;

	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) != 0) {
		return -1.0;
	}
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}


int main(int argc, char *argv[])
{
	static const RpividDrmBufferOps ops = { NULL, kms_alloc, kms_free, kms_wait_idle };
	AVFormatContext *fc = NULL;
	AVCodecContext *cc = NULL;
	const AVCodec *codec;
	AVDictionary *opts = NULL;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *held[HOLD] = { NULL };
	drmModeRes *res;
	uint32_t plane = 0, crtc, mw = 1920u, mh = 1080u, fb;
	long max_frames = 0, decoded = 0, shown = 0;
	int st, ret, done = 0, nopace = 0, ok = 1, i, opt;
	int64_t t0 = 0, pts0 = AV_NOPTS_VALUE;
	double c0, wall, cpu;
	AVRational tb;

	while ((opt = getopt(argc, argv, "n:x")) != -1) {
		if (opt == 'n') {
			max_frames = strtol(optarg, NULL, 0);
		}
		else if (opt == 'x') {
			nopace = 1;
		}
		else {
			fprintf(stderr, "usage: video-plane [-n frames] [-x] file\n");
			return 2;
		}
	}
	if ((optind >= argc) || (pkt == NULL)) {
		fprintf(stderr, "usage: video-plane [-n frames] [-x] file\n");
		return 2;
	}

	card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (card < 0) {
		printf("VIDEOPLANE result=FAIL open /dev/dri/card0: %s\n", strerror(errno));
		return 1;
	}
	(void)drmSetClientCap(card, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	res = drmModeGetResources(card);
	if ((res == NULL) || (res->count_crtcs == 0)) {
		printf("VIDEOPLANE result=FAIL no crtc\n");
		return 1;
	}
	crtc = res->crtcs[0];
	{
		drmModeCrtc *c = drmModeGetCrtc(card, crtc);
		if ((c != NULL) && c->mode_valid) {
			mw = c->mode.hdisplay;
			mh = c->mode.vdisplay;
		}
		drmModeFreeCrtc(c);
	}
	if (!find_plane(&plane)) {
		printf("VIDEOPLANE result=FAIL no plane offers NV12 (rpi4-kms -o)\n");
		return 1;
	}
	rpivid_drm_set_buffer_ops(&ops);

	ret = avformat_open_input(&fc, argv[optind], NULL, NULL);
	if (ret >= 0) {
		ret = avformat_find_stream_info(fc, NULL);
	}
	if (ret < 0) {
		printf("VIDEOPLANE result=FAIL cannot read %s: %s\n", argv[optind], av_err2str(ret));
		return 1;
	}
	st = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	codec = avcodec_find_decoder_by_name("hevc_rpivid");
	if ((st < 0) || (fc->streams[st]->codecpar->codec_id != AV_CODEC_ID_HEVC) || (codec == NULL)) {
		printf("VIDEOPLANE result=FAIL no HEVC stream, or no hevc_rpivid\n");
		return 1;
	}
	cc = avcodec_alloc_context3(codec);
	if ((cc == NULL) || (avcodec_parameters_to_context(cc, fc->streams[st]->codecpar) < 0)) {
		printf("VIDEOPLANE result=FAIL decoder context\n");
		return 1;
	}
	cc->thread_count = 1;
	cc->thread_type = FF_THREAD_SLICE;   /* drm_prime output needs no frame threads */
	cc->pkt_timebase = fc->streams[st]->time_base;
	av_dict_set(&opts, "rpivid_out", "drm_prime", 0);
	ret = avcodec_open2(cc, codec, &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		printf("VIDEOPLANE result=FAIL cannot open hevc_rpivid: %s\n", av_err2str(ret));
		return 1;
	}
	tb = fc->streams[st]->time_base;
	printf("VIDEOPLANE %s: %dx%d, plane %u on crtc %u (%ux%u)\n", argv[optind], cc->width, cc->height, plane, crtc, mw, mh);
	fflush(stdout);

	c0 = cpu_seconds();
	t0 = av_gettime_relative();
	while (!done) {
		AVFrame *f;

		ret = av_read_frame(fc, pkt);
		if (ret < 0) {
			(void)avcodec_send_packet(cc, NULL);   /* drain */
			done = 1;
		}
		else if (pkt->stream_index == st) {
			(void)avcodec_send_packet(cc, pkt);
			av_packet_unref(pkt);
		}
		else {
			av_packet_unref(pkt);
			continue;
		}
		for (;;) {
			f = av_frame_alloc();
			if ((f == NULL) || (avcodec_receive_frame(cc, f) < 0)) {
				av_frame_free(&f);
				break;
			}
			decoded++;
			if ((f->format != AV_PIX_FMT_DRM_PRIME) || (picture_fb(f, &fb) != 0)) {
				if (ok) {
					printf("VIDEOPLANE picture %ld: format %d, no framebuffer\n", decoded, f->format);
				}
				ok = 0;
				av_frame_free(&f);
				continue;
			}
			if (!nopace && (f->pts != AV_NOPTS_VALUE)) {
				int64_t due;

				if (pts0 == AV_NOPTS_VALUE) {
					pts0 = f->pts;
					t0 = av_gettime_relative();
				}
				due = t0 + av_rescale_q(f->pts - pts0, tb, AV_TIME_BASE_Q);
				while (av_gettime_relative() < due) {
					av_usleep((unsigned int)(due - av_gettime_relative()));
				}
			}
			/* fit the picture to the screen, keeping its aspect */
			{
				uint32_t dw = mw, dh = (uint32_t)((uint64_t)mw * (uint32_t)f->height / (uint32_t)f->width);
				if (dh > mh) {
					dh = mh;
					dw = (uint32_t)((uint64_t)mh * (uint32_t)f->width / (uint32_t)f->height);
				}
				if (drmModeSetPlane(card, plane, crtc, fb, 0, (int32_t)(mw - dw) / 2, (int32_t)(mh - dh) / 2, dw, dh, 0, 0,
						(uint32_t)f->width << 16, (uint32_t)f->height << 16) == 0) {
					shown++;
				}
				else if (ok) {
					printf("VIDEOPLANE picture %ld: SETPLANE failed\n", decoded);
					ok = 0;
				}
			}
			av_frame_free(&held[HOLD - 1]);
			for (i = HOLD - 1; i > 0; i--) {
				held[i] = held[i - 1];
			}
			held[0] = f;
			if ((max_frames > 0) && (decoded >= max_frames)) {
				done = 1;
				break;
			}
		}
	}
	wall = (av_gettime_relative() - t0) / 1e6;
	cpu = cpu_seconds() - c0;

	(void)drmModeSetPlane(card, plane, crtc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
	for (i = 0; i < HOLD; i++) {
		av_frame_free(&held[i]);
	}
	avcodec_free_context(&cc);
	avformat_close_input(&fc);
	av_packet_free(&pkt);

	printf("VIDEOPLANE result=%s decoded=%ld shown=%ld wall_s=%.2f cpu_s=%.2f cpu_pct=%.1f fps=%.1f\n",
		(ok && (shown == decoded) && (decoded > 0)) ? "PASS" : "FAIL", decoded, shown, wall, cpu,
		(wall > 0.0) ? 100.0 * cpu / wall : 0.0, (wall > 0.0) ? (double)shown / wall : 0.0);
	return (ok && (shown == decoded)) ? 0 : 1;
}
