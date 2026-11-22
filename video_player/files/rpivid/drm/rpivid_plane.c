/*
 * Phoenix-RTOS
 *
 * hevc_rpivid pictures on a display overlay of rpi4-kms (see rpivid_plane.h)
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
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

#include <libavcodec/rpivid_drm.h>
#include <libavutil/hwcontext_drm.h>

#include "rpivid_plane.h"

#define PAGE    4096u
#define MAX_FBS 48
#define HOLD    3   /* frames referenced: the one shown and the two before it */

static struct {
	pthread_mutex_t lock;   /* fbs: the decoder's thread frees buffers, the player shows them */
	int card;
	uint32_t plane, crtc, mw, mh;
	int coloured;
	struct {
		uint32_t handle, fb;
	} fbs[MAX_FBS];
	int nfbs;
	AVFrame *held[HOLD];
} pl = { PTHREAD_MUTEX_INITIALIZER, -1, 0u, 0u, 1920u, 1080u, 0, { { 0u, 0u } }, 0, { NULL } };


/* ---- the decoder's picture buffers: rpi4-kms dumb buffers ---- */

static void dumb_destroy(uint32_t handle)
{
	struct drm_mode_destroy_dumb dd;

	memset(&dd, 0, sizeof(dd));
	dd.handle = handle;
	(void)drmIoctl(pl.card, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
}


static int buf_alloc(void *opaque, size_t size, RpividDrmBuffer *b)
{
	struct drm_mode_create_dumb cd;
	struct drm_mode_map_dumb md;
	void *p = MAP_FAILED;
	size_t i;
	int fd = -1;

	(void)opaque;
	size = (size + PAGE - 1u) & ~(size_t)(PAGE - 1u);
	memset(&cd, 0, sizeof(cd));
	cd.width = PAGE;   /* 8-bit "pixels", one page per row */
	cd.height = (uint32_t)(size / PAGE);
	cd.bpp = 8u;
	if (drmIoctl(pl.card, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0) {
		return -errno;
	}
	memset(&md, 0, sizeof(md));
	md.handle = cd.handle;
	if (drmIoctl(pl.card, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0) {
		p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, pl.card, (off_t)md.offset);
	}
	if (p == MAP_FAILED) {
		dumb_destroy(cd.handle);
		return -ENOMEM;
	}
	/* the block has no MMU: one physically contiguous range (the pool's buffers are) */
	b->pa = (uint64_t)va2pa(p);
	for (i = PAGE; (b->pa != 0u) && (i < size); i += PAGE) {
		if ((uint64_t)va2pa((uint8_t *)p + i) != b->pa + i) {
			b->pa = 0;
		}
	}
	if ((b->pa == 0u) || (drmPrimeHandleToFD(pl.card, cd.handle, DRM_CLOEXEC, &fd) != 0)) {
		munmap(p, size);
		dumb_destroy(cd.handle);
		return (b->pa == 0u) ? -EFAULT : -errno;
	}
	b->cpu = p;
	b->size = size;
	b->fd = fd;
	b->handle = cd.handle;
	b->priv = NULL;
	return 0;
}


static void buf_free(void *opaque, RpividDrmBuffer *b)
{
	int i;

	(void)opaque;
	pthread_mutex_lock(&pl.lock);
	for (i = 0; i < pl.nfbs; i++) {
		if (pl.fbs[i].handle == b->handle) {
			(void)drmModeRmFB(pl.card, pl.fbs[i].fb);
			pl.fbs[i] = pl.fbs[--pl.nfbs];
			break;
		}
	}
	pthread_mutex_unlock(&pl.lock);
	if (b->cpu != NULL) {
		munmap(b->cpu, b->size);
	}
	if (b->fd >= 0) {
		close(b->fd);
	}
	dumb_destroy(b->handle);
	memset(b, 0, sizeof(*b));
	b->fd = -1;
}


static int buf_wait_idle(void *opaque, const RpividDrmBuffer *b)
{
	(void)opaque;
	(void)b;
	return 0;   /* no GPU reads them; the plane is covered by HOLD */
}


/* ---- the overlay ---- */

static int find_plane(void)
{
	drmModePlaneRes *pr = drmModeGetPlaneResources(pl.card);
	uint32_t i, j;
	int found = 0;

	for (i = 0; (pr != NULL) && (i < pr->count_planes) && !found; i++) {
		drmModePlane *p = drmModeGetPlane(pl.card, pr->planes[i]);
		for (j = 0; (p != NULL) && (j < p->count_formats); j++) {
			if ((p->formats[j] == DRM_FORMAT_NV12) && ((p->possible_crtcs & 1u) != 0u)) {
				pl.plane = p->plane_id;
				found = 1;
				break;
			}
		}
		drmModeFreePlane(p);
	}
	drmModeFreePlaneResources(pr);
	return found;
}


/* The overlay's property ids, by name (0 = the server has no such property) */
enum { P_FB_ID, P_CRTC_ID, P_SRC_X, P_SRC_Y, P_SRC_W, P_SRC_H, P_CRTC_X, P_CRTC_Y, P_CRTC_W, P_CRTC_H, P_ENC, P_RANGE, P_N };
static const char *const pnames[P_N] = { "FB_ID", "CRTC_ID", "SRC_X", "SRC_Y", "SRC_W", "SRC_H", "CRTC_X", "CRTC_Y",
	"CRTC_W", "CRTC_H", "COLOR_ENCODING", "COLOR_RANGE" };
static uint32_t pids[P_N];


static void find_props(void)
{
	drmModeObjectProperties *pr = drmModeObjectGetProperties(pl.card, pl.plane, DRM_MODE_OBJECT_PLANE);
	uint32_t i, k;

	for (i = 0; (pr != NULL) && (i < pr->count_props); i++) {
		drmModePropertyRes *p = drmModeGetProperty(pl.card, pr->props[i]);
		for (k = 0; (p != NULL) && (k < P_N); k++) {
			if (strcmp(p->name, pnames[k]) == 0) {
				pids[k] = p->prop_id;
			}
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(pr);
}


/* COLOR_ENCODING / COLOR_RANGE from the stream's tags (DRM enum values: BT.601 0, BT.709 1,
 * BT.2020 2; limited 0, full 1). Untagged: BT.709 from 720 lines up, else BT.601. */
static void stream_colour(const AVFrame *f, uint64_t *enc, uint64_t *range)
{
	*range = (f->color_range == AVCOL_RANGE_JPEG) ? 1u : 0u;
	switch (f->colorspace) {
		case AVCOL_SPC_BT709: *enc = 1u; break;
		case AVCOL_SPC_BT470BG:
		case AVCOL_SPC_SMPTE170M: *enc = 0u; break;
		case AVCOL_SPC_BT2020_NCL:
		case AVCOL_SPC_BT2020_CL: *enc = 2u; break;
		default: *enc = (f->height >= 720) ? 1u : 0u; break;
	}
}


/* The first picture: ONE atomic commit with the colour properties and the plane state.
 * (Setting each property on its own is a commit per property, and the next one finds the
 * previous still in flight: -EBUSY.) */
static int first_commit(const AVFrame *f, uint32_t fb, int x, int y, int w, int h)
{
	drmModeAtomicReq *rq = drmModeAtomicAlloc();
	uint64_t enc, range;
	int rc;

	if (rq == NULL) {
		return -ENOMEM;
	}
	stream_colour(f, &enc, &range);
	(void)drmModeAtomicAddProperty(rq, pl.plane, pids[P_FB_ID], fb);
	(void)drmModeAtomicAddProperty(rq, pl.plane, pids[P_CRTC_ID], pl.crtc);
	(void)drmModeAtomicAddProperty(rq, pl.plane, pids[P_SRC_X], 0);
	(void)drmModeAtomicAddProperty(rq, pl.plane, pids[P_SRC_Y], 0);
	(void)drmModeAtomicAddProperty(rq, pl.plane, pids[P_SRC_W], (uint64_t)f->width << 16);
	(void)drmModeAtomicAddProperty(rq, pl.plane, pids[P_SRC_H], (uint64_t)f->height << 16);
	(void)drmModeAtomicAddProperty(rq, pl.plane, pids[P_CRTC_X], (uint64_t)(int64_t)x);
	(void)drmModeAtomicAddProperty(rq, pl.plane, pids[P_CRTC_Y], (uint64_t)(int64_t)y);
	(void)drmModeAtomicAddProperty(rq, pl.plane, pids[P_CRTC_W], (uint64_t)w);
	(void)drmModeAtomicAddProperty(rq, pl.plane, pids[P_CRTC_H], (uint64_t)h);
	if (pids[P_ENC] != 0u) {
		(void)drmModeAtomicAddProperty(rq, pl.plane, pids[P_ENC], enc);
	}
	if (pids[P_RANGE] != 0u) {
		(void)drmModeAtomicAddProperty(rq, pl.plane, pids[P_RANGE], range);
	}
	rc = drmModeAtomicCommit(pl.card, rq, 0, NULL);
	drmModeAtomicFree(rq);
	fprintf(stderr, "rpivid-plane: colorspace %d range %d -> COLOR_ENCODING %llu COLOR_RANGE %llu: rc %d\n",
		(int)f->colorspace, (int)f->color_range, (unsigned long long)enc, (unsigned long long)range, rc);
	return rc;
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
	if (drmPrimeFDToHandle(pl.card, d->objects[0].fd, &h) != 0) {
		return -errno;
	}
	pthread_mutex_lock(&pl.lock);
	for (i = 0; i < pl.nfbs; i++) {
		if (pl.fbs[i].handle == h) {
			*fb = pl.fbs[i].fb;
			pthread_mutex_unlock(&pl.lock);
			return 0;
		}
	}
	rc = -ENOSPC;
	if (pl.nfbs < MAX_FBS) {
		handles[0] = handles[1] = h;
		pitches[0] = (uint32_t)d->layers[0].planes[0].pitch;
		pitches[1] = (uint32_t)d->layers[0].planes[1].pitch;
		offsets[0] = (uint32_t)d->layers[0].planes[0].offset;
		offsets[1] = (uint32_t)d->layers[0].planes[1].offset;
		mods[0] = mods[1] = d->objects[0].format_modifier;
		rc = drmModeAddFB2WithModifiers(pl.card, (uint32_t)f->width, (uint32_t)f->height, DRM_FORMAT_NV12, handles,
			pitches, offsets, mods, fb, DRM_MODE_FB_MODIFIERS);
		if (rc == 0) {
			pl.fbs[pl.nfbs].handle = h;
			pl.fbs[pl.nfbs].fb = *fb;
			pl.nfbs++;
		}
	}
	pthread_mutex_unlock(&pl.lock);
	return rc;
}


int rpivid_plane_open(void)
{
	static const RpividDrmBufferOps ops = { NULL, buf_alloc, buf_free, buf_wait_idle };
	drmModeRes *res;

	if (pl.card >= 0) {
		return 0;
	}
	pl.card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (pl.card < 0) {
		return -errno;
	}
	(void)drmSetClientCap(pl.card, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	(void)drmSetClientCap(pl.card, DRM_CLIENT_CAP_ATOMIC, 1);
	res = drmModeGetResources(pl.card);
	if ((res == NULL) || (res->count_crtcs == 0) || !find_plane()) {
		drmModeFreeResources(res);
		close(pl.card);
		pl.card = -1;
		return -ENODEV;
	}
	pl.crtc = res->crtcs[0];
	drmModeFreeResources(res);
	find_props();
	{
		drmModeCrtc *c = drmModeGetCrtc(pl.card, pl.crtc);
		if ((c != NULL) && c->mode_valid) {
			pl.mw = c->mode.hdisplay;
			pl.mh = c->mode.vdisplay;
		}
		drmModeFreeCrtc(c);
	}
	rpivid_drm_set_buffer_ops(&ops);
	fprintf(stderr, "rpivid-plane: overlay %u on crtc %u\n", pl.plane, pl.crtc);
	return 0;
}


int rpivid_plane_show(const AVFrame *frame, int x, int y, int w, int h)
{
	AVFrame *ref;
	uint32_t fb;
	int i, rc;

	if ((pl.card < 0) || (frame->format != AV_PIX_FMT_DRM_PRIME) || (w <= 0) || (h <= 0)) {
		return -EINVAL;
	}
	rc = picture_fb(frame, &fb);
	if (rc != 0) {
		return rc;
	}
	ref = av_frame_clone(frame);
	if (ref == NULL) {
		return -ENOMEM;
	}
	/* -EBUSY: the previous picture's flip is still in flight (pictures faster than the
	 * vblank); wait for it a little, as a blocking legacy call would, rather than drop */
	for (i = 0; i < 20; i++) {
		if (!pl.coloured) {
			rc = first_commit(frame, fb, x, y, w, h);
			pl.coloured = (rc == 0) ? 1 : 0;
		}
		else {
			rc = drmModeSetPlane(pl.card, pl.plane, pl.crtc, fb, 0, x, y, (uint32_t)w, (uint32_t)h, 0, 0,
				(uint32_t)frame->width << 16, (uint32_t)frame->height << 16);
		}
		if ((rc != -EBUSY) && !((rc == -1) && (errno == EBUSY))) {
			break;
		}
		{
			struct timespec ts = { 0, 2000000 };
			nanosleep(&ts, NULL);
		}
	}
	if (rc != 0) {
		av_frame_free(&ref);
		return rc;
	}
	av_frame_free(&pl.held[HOLD - 1]);
	for (i = HOLD - 1; i > 0; i--) {
		pl.held[i] = pl.held[i - 1];
	}
	pl.held[0] = ref;
	return 0;
}


void rpivid_plane_screen(uint32_t *w, uint32_t *h)
{
	*w = pl.mw;
	*h = pl.mh;
}


void rpivid_plane_close(void)
{
	int i;

	if (pl.card < 0) {
		return;
	}
	(void)drmModeSetPlane(pl.card, pl.plane, pl.crtc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
	for (i = 0; i < HOLD; i++) {
		av_frame_free(&pl.held[i]);
	}
}


/* VIDEO_PLANE=1 in the environment: open the overlay before main() (ffplay opens its decoder
 * long before it shows anything, and the decoder must get these buffers from the start). */
__attribute__((constructor)) static void rpivid_plane_env(void)
{
	const char *e = getenv("VIDEO_PLANE");

	if ((e != NULL) && (e[0] == '1')) {
		if (rpivid_plane_open() == 0) {
			atexit(rpivid_plane_close);
		}
		else {
			fprintf(stderr, "rpivid-plane: VIDEO_PLANE=1 but no overlay that takes NV12: the usual display path\n");
		}
	}
}
