/*
 * Phoenix-RTOS
 *
 * hevc_rpivid pictures in a Wayland window (see rpivid_wl.h)
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <drm_fourcc.h>
#include <wayland-client.h>

#include <libavutil/hwcontext_drm.h>

#include "linux-dmabuf-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

#include "rpivid_plane.h"
#include "rpivid_wl.h"

#define MAX_BUFS 48

struct slot {
	int fd;                     /* the decoder buffer's dma-buf descriptor: the key (-1 = free) */
	struct wl_buffer *buffer;
	AVFrame *ref;               /* the picture while the compositor may read the buffer */
};

static struct {
	pthread_mutex_t lock;       /* slots: the decoder's thread frees buffers (forget()) */
	struct wl_display *dpy;
	struct wl_event_queue *q;
	struct wl_compositor *compositor;
	struct wl_subcompositor *subcompositor;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	struct wp_viewporter *viewporter;
	struct wl_surface *surface;
	struct wl_subsurface *sub;
	struct wp_viewport *viewport;
	int sand, open, shown;
	int x, y, w, h;
	struct slot slots[MAX_BUFS];
} wl = { .lock = PTHREAD_MUTEX_INITIALIZER };


static void dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *d, uint32_t format)
{
	(void)data;
	(void)d;
	(void)format;
}


static void dmabuf_modifier(void *data, struct zwp_linux_dmabuf_v1 *d, uint32_t format, uint32_t hi, uint32_t lo)
{
	(void)data;
	(void)d;
	if ((format == DRM_FORMAT_NV12) &&
			(fourcc_mod_broadcom_mod(((uint64_t)hi << 32) | lo) == DRM_FORMAT_MOD_BROADCOM_SAND128)) {
		wl.sand = 1;
	}
}


static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = { dmabuf_format, dmabuf_modifier };


static void global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
	(void)data;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		wl.compositor = wl_registry_bind(r, name, &wl_compositor_interface, 1);
	}
	else if (strcmp(iface, wl_subcompositor_interface.name) == 0) {
		wl.subcompositor = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
	}
	else if ((strcmp(iface, zwp_linux_dmabuf_v1_interface.name) == 0) && (ver >= 3u)) {
		wl.dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, 3);
		zwp_linux_dmabuf_v1_add_listener(wl.dmabuf, &dmabuf_listener, NULL);
	}
	else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
		wl.viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
	}
}


static void global_remove(void *data, struct wl_registry *r, uint32_t name)
{
	(void)data;
	(void)r;
	(void)name;
}


static const struct wl_registry_listener registry_listener = { global, global_remove };


/* The compositor is done with a buffer: the decoder may reuse the picture's memory */
static void buffer_release(void *data, struct wl_buffer *b)
{
	struct slot *s = data;

	(void)b;
	pthread_mutex_lock(&wl.lock);
	av_frame_free(&s->ref);
	pthread_mutex_unlock(&wl.lock);
}


static const struct wl_buffer_listener buffer_listener = { buffer_release };


/* A freed decoder buffer (rpivid_plane.c, the decoder's thread): drop its compositor buffer */
static void forget(int fd)
{
	int i;

	pthread_mutex_lock(&wl.lock);
	for (i = 0; i < MAX_BUFS; i++) {
		if (wl.slots[i].fd == fd) {
			if (wl.slots[i].buffer != NULL) {
				wl_buffer_destroy(wl.slots[i].buffer);
			}
			av_frame_free(&wl.slots[i].ref);
			wl.slots[i].buffer = NULL;
			wl.slots[i].fd = -1;
		}
	}
	pthread_mutex_unlock(&wl.lock);
}


struct create_result {
	struct wl_buffer *buffer;
	int done;
};


static void params_created(void *data, struct zwp_linux_buffer_params_v1 *p, struct wl_buffer *b)
{
	struct create_result *r = data;

	(void)p;
	r->buffer = b;
	r->done = 1;
}


static void params_failed(void *data, struct zwp_linux_buffer_params_v1 *p)
{
	struct create_result *r = data;

	(void)p;
	r->done = -1;
}


static const struct zwp_linux_buffer_params_v1_listener params_listener = { params_created, params_failed };


/* The slot of a picture's buffer, its compositor buffer created on first use */
static struct slot *picture_slot(const AVFrame *f)
{
	const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *)f->data[0];
	struct zwp_linux_buffer_params_v1 *params;
	struct create_result r = { NULL, 0 };
	struct slot *s = NULL;
	uint64_t mod;
	int i, fd;

	if ((d == NULL) || (d->nb_objects != 1) || (d->nb_layers != 1) || (d->layers[0].nb_planes != 2)) {
		return NULL;
	}
	fd = d->objects[0].fd;
	pthread_mutex_lock(&wl.lock);
	for (i = 0; i < MAX_BUFS; i++) {
		if (wl.slots[i].fd == fd) {
			pthread_mutex_unlock(&wl.lock);
			return &wl.slots[i];
		}
		if ((s == NULL) && (wl.slots[i].fd < 0)) {
			s = &wl.slots[i];
		}
	}
	pthread_mutex_unlock(&wl.lock);
	if (s == NULL) {
		return NULL;
	}

	mod = d->objects[0].format_modifier;
	params = zwp_linux_dmabuf_v1_create_params(wl.dmabuf);
	zwp_linux_buffer_params_v1_add_listener(params, &params_listener, &r);
	for (i = 0; i < 2; i++) {
		zwp_linux_buffer_params_v1_add(params, fd, (uint32_t)i, (uint32_t)d->layers[0].planes[i].offset,
			(uint32_t)d->layers[0].planes[i].pitch, (uint32_t)(mod >> 32), (uint32_t)mod);
	}
	zwp_linux_buffer_params_v1_create(params, f->width, f->height, DRM_FORMAT_NV12, 0);
	while ((r.done == 0) && (wl_display_roundtrip_queue(wl.dpy, wl.q) >= 0)) {
	}
	zwp_linux_buffer_params_v1_destroy(params);
	if (r.done != 1) {
		fprintf(stderr, "rpivid-wl: the compositor refused a %dx%d NV12 buffer (modifier 0x%016llx)\n", f->width,
			f->height, (unsigned long long)mod);
		return NULL;
	}
	wl_buffer_add_listener(r.buffer, &buffer_listener, s);
	pthread_mutex_lock(&wl.lock);
	s->fd = fd;
	s->buffer = r.buffer;
	s->ref = NULL;
	pthread_mutex_unlock(&wl.lock);
	return s;
}


int rpivid_wl_open(struct wl_display *display, struct wl_surface *parent)
{
	struct wl_display *wrapper;
	struct wl_registry *registry;
	struct wl_region *none;
	int i;

	if (wl.open) {
		return 0;
	}
	if ((display == NULL) || (parent == NULL)) {
		return -EINVAL;
	}
	for (i = 0; i < MAX_BUFS; i++) {
		wl.slots[i].fd = -1;
	}
	wl.dpy = display;
	wl.q = wl_display_create_queue(display);
	wrapper = wl_proxy_create_wrapper(display);
	if ((wl.q == NULL) || (wrapper == NULL)) {
		return -ENOMEM;
	}
	wl_proxy_set_queue((struct wl_proxy *)wrapper, wl.q);
	registry = wl_display_get_registry(wrapper);
	wl_proxy_wrapper_destroy(wrapper);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	(void)wl_display_roundtrip_queue(display, wl.q);
	(void)wl_display_roundtrip_queue(display, wl.q);   /* the dmabuf modifier events */
	if ((wl.compositor == NULL) || (wl.subcompositor == NULL) || (wl.dmabuf == NULL) || (wl.viewporter == NULL) ||
			!wl.sand) {
		fprintf(stderr, "rpivid-wl: compositor=%d subcompositor=%d linux-dmabuf-v3=%d viewporter=%d nv12-sand128=%d: "
			"not used\n", wl.compositor != NULL, wl.subcompositor != NULL, wl.dmabuf != NULL, wl.viewporter != NULL, wl.sand);
		return -ENOTSUP;
	}

	wl.surface = wl_compositor_create_surface(wl.compositor);
	wl.sub = wl_subcompositor_get_subsurface(wl.subcompositor, wl.surface, parent);
	wl_subsurface_set_desync(wl.sub);   /* pictures at the player's pace, not the window's */
	wl.viewport = wp_viewporter_get_viewport(wl.viewporter, wl.surface);
	none = wl_compositor_create_region(wl.compositor);
	wl_surface_set_input_region(wl.surface, none);   /* pointer and keys stay the window's */
	wl_region_destroy(none);
	rpivid_plane_buffer_freed = forget;
	wl.open = 1;
	fprintf(stderr, "rpivid-wl: pictures on a subsurface (NV12 SAND128 dma-bufs)\n");
	return 0;
}


int rpivid_wl_show(const AVFrame *frame, int x, int y, int w, int h)
{
	struct slot *s;
	AVFrame *ref = NULL;

	if (!wl.open || (frame->format != AV_PIX_FMT_DRM_PRIME) || (w <= 0) || (h <= 0)) {
		return -EINVAL;
	}
	(void)wl_display_dispatch_queue_pending(wl.dpy, wl.q);   /* buffer releases */
	s = picture_slot(frame);
	if (s == NULL) {
		return -EIO;
	}
	pthread_mutex_lock(&wl.lock);
	if (s->ref == NULL) {
		/* A buffer the compositor still holds can only be this same picture shown again
		 * (the decoder reuses it only after every reference is gone): one reference is enough */
		ref = av_frame_clone(frame);
		s->ref = ref;
	}
	pthread_mutex_unlock(&wl.lock);

	if ((x != wl.x) || (y != wl.y) || !wl.shown) {
		wl_subsurface_set_position(wl.sub, x, y);   /* applies with the window's next commit */
		wl.x = x;
		wl.y = y;
	}
	if ((w != wl.w) || (h != wl.h) || !wl.shown) {
		wp_viewport_set_destination(wl.viewport, w, h);
		wl.w = w;
		wl.h = h;
	}
	wl_surface_attach(wl.surface, s->buffer, 0, 0);
	wl_surface_damage(wl.surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(wl.surface);
	(void)wl_display_flush(wl.dpy);
	if (!wl.shown) {
		fprintf(stderr, "rpivid-wl: first picture %dx%d at %d,%d %dx%d\n", frame->width, frame->height, x, y, w, h);
	}
	wl.shown = 1;
	return 0;
}


void rpivid_wl_hide(void)
{
	if (!wl.open || !wl.shown) {
		return;
	}
	wl_surface_attach(wl.surface, NULL, 0, 0);
	wl_surface_commit(wl.surface);
	(void)wl_display_flush(wl.dpy);
	wl.shown = 0;
}
