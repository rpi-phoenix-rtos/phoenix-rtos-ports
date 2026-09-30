/*
 * Phoenix-RTOS
 *
 * libdrm-phoenix - the display node (/dev/dri/card0 -> rpi4-kms)
 *
 * Marshals the DRM KMS uapi into rpi4-kms requests (kms_proto.h). Scalars travel
 * in the 64-byte raw envelope; arrays (modes, property lists, plane states,
 * blobs) through the connection's page-aligned bounce buffer. Events are not
 * handled here at all: drmHandleEvent() read()s the card descriptor and the
 * server answers in the DRM event wire layout.
 *
 * Library-side semantics the server leaves to M3 (M2 section 11):
 *   - ATOMIC: DRM requests are partial, kms_atomic_plane_t is complete. Each
 *     plane's last committed state is mirrored per connection (seeded from
 *     GET_PROPERTIES) and the request is overlaid on it.
 *   - blocking commits (SETCRTC, SETPLANE, ATOMIC without NONBLOCK) wait here
 *     until the server no longer reports a pending flip.
 *   - legacy ADDFB -> ADDFB2, SETPLANE / OBJ_SETPROPERTY -> one-plane atomic.
 *   - PRIME export = KMS_OP_PRIME_EXPORT + open("/kmsbuf/<id>").
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include "drm_phoenix_priv.h"
#include "drm_fourcc.h"


#define NR(r) DRMPHX_IOC_NR(r)
#define KMS_MAX_ARRAY 64u     /* ids / props / modes returned per request (the server has fewer) */
#define KMS_MAX_ENUMS 16u


static int kcall(drmphx_conn_t *c, uint32_t op, const void *u, size_t usize, kms_resp_t *r, const void *idata,
	size_t isize, void *odata, size_t osize)
{
	kms_req_t rq;
	kms_resp_t tmp;

	memset(&rq, 0, sizeof(rq));
	rq.magic = KMS_MAGIC;
	rq.op = op;
	if ((u != NULL) && (usize != 0u)) {
		memcpy(&rq.u, u, usize);
	}
	return drmphx_call(c, &rq, (r != NULL) ? r : &tmp, idata, isize, isize, odata, osize);
}


int drmphx_kms_hello(drmphx_conn_t *c, int fd)
{
	kms_hello_t h;
	int rc;

	memset(&h, 0, sizeof(h));
	h.proto = KMS_PROTO_VERSION;
	rc = ioctl(fd, KMS_IOC_HELLO, &h);
	if ((rc < 0) && (errno == EPROTO)) {
		/* a proto-1 server (before G7) takes exactly 1: everything but PRIME_IMPORT works */
		memset(&h, 0, sizeof(h));
		h.proto = KMS_PROTO_BASE;
		rc = ioctl(fd, KMS_IOC_HELLO, &h);
	}
	if (rc < 0) {
		return (errno != 0) ? -errno : -EIO;
	}
	if (h.client_id == 0u) {
		return -EPROTO;
	}
	c->oid.id = h.client_id;
	c->u.kms.hello = h;
	c->u.kms.buf_port = h.buf_port;
	return 0;
}


/* ========================================================================= */
/* Local tables                                                               */
/* ========================================================================= */

static drmphx_kms_dumb_t *dumb_find(drmphx_conn_t *c, uint32_t handle)
{
	uint32_t i;

	for (i = 0u; (handle != 0u) && (i < DRMPHX_KMS_MAX_DUMB); i++) {
		if (c->u.kms.dumb[i].handle == handle) {
			return &c->u.kms.dumb[i];
		}
	}
	return NULL;
}


static void dumb_store(drmphx_conn_t *c, const kms_dumb_resp_t *d)
{
	drmphx_kms_dumb_t *e;
	uint32_t i;

	(void)pthread_mutex_lock(&c->lock);
	e = dumb_find(c, d->handle);
	for (i = 0u; (e == NULL) && (i < DRMPHX_KMS_MAX_DUMB); i++) {
		if (c->u.kms.dumb[i].handle == 0u) {
			e = &c->u.kms.dumb[i];
		}
	}
	if (e != NULL) {
		e->handle = d->handle;
		e->pitch = d->pitch;
		e->size = d->size;
		e->mem = d->mem;
	}
	(void)pthread_mutex_unlock(&c->lock);
}


static void dumb_drop(drmphx_conn_t *c, uint32_t handle)
{
	drmphx_kms_dumb_t *e;

	(void)pthread_mutex_lock(&c->lock);
	e = dumb_find(c, handle);
	if (e != NULL) {
		memset(e, 0, sizeof(*e));
	}
	(void)pthread_mutex_unlock(&c->lock);
}


/* The memref behind a dumb handle: cached from CREATE_DUMB, else MAP_DUMB. */
int drmphx_kms_token_memref(drmphx_conn_t *c, uint32_t handle, kms_memref_t *m)
{
	kms_handle_req_t q;
	kms_resp_t r;
	drmphx_kms_dumb_t *e;
	int rc;

	(void)pthread_mutex_lock(&c->lock);
	e = dumb_find(c, handle);
	if ((e != NULL) && (e->mem.kind != KMS_MEM_NONE)) {
		*m = e->mem;
		(void)pthread_mutex_unlock(&c->lock);
		return 0;
	}
	(void)pthread_mutex_unlock(&c->lock);

	memset(&q, 0, sizeof(q));
	q.handle = handle;
	rc = kcall(c, KMS_OP_MAP_DUMB, &q, sizeof(q), &r, NULL, 0u, NULL, 0u);
	if (rc != 0) {
		return rc;
	}
	dumb_store(c, &r.u.dumb);
	*m = r.u.dumb.mem;
	return 0;
}


/* G13: framebuffer -> the dumb handle it scans (ADDFB2 names exactly one) and that
 * handle's buffer. The buffer is recorded here, not looked up at flip time: in DRM
 * the framebuffer holds its buffer, and a compositor may close the handle right
 * after ADDFB2 (wlroots does, backend/drm/fb.c) - its flips must stay gated (G5). */
static void fb_note(drmphx_conn_t *c, uint32_t fb_id, uint32_t handle, const kms_memref_t *mem)
{
	static int warned;
	uint32_t i, k = DRMPHX_KMS_MAX_FB;

	(void)pthread_mutex_lock(&c->lock);
	for (i = 0u; i < DRMPHX_KMS_MAX_FB; i++) {
		if (c->u.kms.fb[i].fb_id == fb_id) {
			k = i;
			break;
		}
		if ((c->u.kms.fb[i].fb_id == 0u) && (k == DRMPHX_KMS_MAX_FB)) {
			k = i;
		}
	}
	if (k < DRMPHX_KMS_MAX_FB) {
		c->u.kms.fb[k].fb_id = fb_id;
		c->u.kms.fb[k].handle = handle;
		c->u.kms.fb[k].mem = *mem;
	}
	(void)pthread_mutex_unlock(&c->lock);
	if ((k == DRMPHX_KMS_MAX_FB) && (warned == 0)) {
		warned = 1;
		(void)fprintf(stderr, "libdrm-phoenix: more than %u framebuffers: flips of fb %u go without implicit sync\n",
			DRMPHX_KMS_MAX_FB, fb_id);
	}
}


static void fb_drop(drmphx_conn_t *c, uint32_t fb_id)
{
	uint32_t i;

	(void)pthread_mutex_lock(&c->lock);
	for (i = 0u; i < DRMPHX_KMS_MAX_FB; i++) {
		if (c->u.kms.fb[i].fb_id == fb_id) {
			memset(&c->u.kms.fb[i], 0, sizeof(c->u.kms.fb[i]));
		}
	}
	(void)pthread_mutex_unlock(&c->lock);
}


/* The buffer export {namespace port, id} a framebuffer scans, if it has one (a
 * pool dumb BO or an alias of one: the /kmsbuf name; a G7 import: the /v3dbuf
 * name). 0 or -ENOENT. */
static int fb_export(drmphx_conn_t *c, uint32_t fb_id, uint32_t *port, uint64_t *id)
{
	uint32_t i;
	int rc = -ENOENT;

	(void)pthread_mutex_lock(&c->lock);
	for (i = 0u; (fb_id != 0u) && (i < DRMPHX_KMS_MAX_FB); i++) {
		if (c->u.kms.fb[i].fb_id == fb_id) {
			if (c->u.kms.fb[i].mem.kind == KMS_MEM_OID) {
				*port = c->u.kms.fb[i].mem.port;
				*id = c->u.kms.fb[i].mem.addr;
				rc = 0;
			}
			break;
		}
	}
	(void)pthread_mutex_unlock(&c->lock);
	return rc;
}


/* G13 (M3 part 2): a flip of a GPU-rendered buffer that carries no IN_FENCE_FD
 * must not scan out a frame the GPU is still writing (Linux waits on the buffer's
 * dma-resv; kmscube's legacy path flips with no fence). When this process imported
 * the framebuffer's buffer on the render node, attach that BO's last-use fence -
 * mirrored from its own submits, no IPC - and rpi4-kms -G gates the flip on the
 * render fence page. Nothing is attached when the fence already passed, so an
 * idle buffer flips exactly as before. Returns 1 when a fence was attached. */
static int implicit_attach(drmphx_conn_t *c, uint32_t fb_id, kms_fence_t *in_fence)
{
	v3da_fence_t f;
	uint32_t port;
	uint64_t id;

	if ((in_fence->seqno != 0u) || (fb_export(c, fb_id, &port, &id) != 0)) {
		return 0;   /* an explicit fence wins; a buffer without an export has no importer */
	}
	/* G6: a render BO imported here from another process (G7, e.g. a Wayland client's
	 * buffer on a plane) waits for every client's last use, which the render server
	 * answers; the display server's own dumb buffers keep the G13 in-process mirror. */
	if (c->u.kms.no_gate != 0) {
		(void)drmphx_v3d_flip_wait(c->u.kms.buf_port, port, id);   /* no -G: the CPU waits instead of the server */
		return 0;
	}
	if (drmphx_v3d_flip_fence(c->u.kms.buf_port, port, id, &f) == 0) {
		return 0;
	}
	memcpy(in_fence, &f, sizeof(*in_fence));   /* identical layouts (kms_proto.h) */
	return 1;
}


/* rpi4-kms started without -G refuses every in-fence with -ENODEV. For implicit
 * fences only: remember it (once per connection), wait on the CPU, retry bare. */
static void implicit_fallback(drmphx_conn_t *c, uint32_t fb_id, kms_fence_t *in_fence)
{
	uint32_t port;
	uint64_t id;

	if (c->u.kms.no_gate == 0) {
		c->u.kms.no_gate = 1;
		(void)fprintf(stderr, "libdrm-phoenix: rpi4-kms runs without -G: implicit flip sync falls back to CPU waits\n");
	}
	if (fb_export(c, fb_id, &port, &id) == 0) {
		(void)drmphx_v3d_flip_wait(c->u.kms.buf_port, port, id);
	}
	memset(in_fence, 0, sizeof(*in_fence));
}


static void mirror_invalidate(drmphx_conn_t *c)
{
	(void)pthread_mutex_lock(&c->lock);
	memset(c->u.kms.plane_valid, 0, sizeof(c->u.kms.plane_valid));
	(void)pthread_mutex_unlock(&c->lock);
}


static void mirror_store(drmphx_conn_t *c, const kms_atomic_plane_t *st, uint32_t n)
{
	uint32_t i, idx;

	(void)pthread_mutex_lock(&c->lock);
	for (i = 0u; i < n; i++) {
		idx = st[i].plane_id - KMS_ID_PLANE(0u, 0u);
		if (idx < DRMPHX_KMS_MAX_PLANES) {
			c->u.kms.plane[idx] = st[i];
			memset(&c->u.kms.plane[idx].in_fence, 0, sizeof(kms_fence_t));
			if (st[i].fb_id == 0u) {
				c->u.kms.plane[idx].crtc_id = 0u;   /* the server unbinds a disabled plane */
			}
			c->u.kms.plane_valid[idx] = 1u;
		}
	}
	(void)pthread_mutex_unlock(&c->lock);
}


/* drmphx_plane_state_fn: the mirror, else the server's current properties. */
static int plane_baseline(void *ctx, uint32_t plane_id, kms_atomic_plane_t *st)
{
	drmphx_conn_t *c = ctx;
	kms_prop_value_t pv[KMS_MAX_ARRAY];
	kms_obj_req_t q;
	kms_resp_t r;
	uint32_t idx = plane_id - KMS_ID_PLANE(0u, 0u), n;
	int rc;

	if (idx >= DRMPHX_KMS_MAX_PLANES) {
		return -ENOENT;
	}
	(void)pthread_mutex_lock(&c->lock);
	if (c->u.kms.plane_valid[idx] != 0u) {
		*st = c->u.kms.plane[idx];
		(void)pthread_mutex_unlock(&c->lock);
		return 0;
	}
	(void)pthread_mutex_unlock(&c->lock);

	memset(&q, 0, sizeof(q));
	q.id = plane_id;
	q.type = KMS_OBJ_PLANE;
	q.max = KMS_MAX_ARRAY;
	rc = kcall(c, KMS_OP_GET_PROPERTIES, &q, sizeof(q), &r, NULL, 0u, pv, sizeof(pv));
	if (rc != 0) {
		return rc;
	}
	n = (r.u.count < KMS_MAX_ARRAY) ? r.u.count : KMS_MAX_ARRAY;
	drmphx_plane_state_from_props(plane_id, pv, n, st);
	if (st->rotation == 0u) {
		st->rotation = DRM_MODE_ROTATE_0;
	}
	mirror_store(c, st, 1u);
	return 0;
}


/* ========================================================================= */
/* Commits                                                                    */
/* ========================================================================= */

/* Blocking-commit completion: wait for the vblank after acceptance, then until
 * the server reports no pending flip (bounded; Stage A exposes the primary's
 * pending framebuffer only - a pending flag is on the server work list). */
static void wait_commit_done(drmphx_conn_t *c, uint32_t crtc_id, uint64_t accepted_seq)
{
	kms_vblank_req_t v;
	kms_obj_req_t q;
	kms_resp_t r;
	int i;

	memset(&v, 0, sizeof(v));
	v.crtc_id = crtc_id;
	v.type = KMS_VBL_ABSOLUTE;   /* bit 0 = absolute in the server; kms_proto.h pairs the names with the wrong _DRM_VBLANK_* in comments */
	v.sequence = accepted_seq + 1u;
	(void)kcall(c, KMS_OP_WAIT_VBLANK, &v, sizeof(v), &r, NULL, 0u, NULL, 0u);
	for (i = 0; i < 4; i++) {
		memset(&q, 0, sizeof(q));
		q.id = crtc_id;
		if ((kcall(c, KMS_OP_GET_CRTC, &q, sizeof(q), &r, NULL, 0u, NULL, 0u) != 0) || (r.u.crtc.pending_fb == 0u)) {
			return;
		}
		v.type = KMS_VBL_RELATIVE;
		v.sequence = 1u;
		(void)kcall(c, KMS_OP_WAIT_VBLANK, &v, sizeof(v), &r, NULL, 0u, NULL, 0u);
	}
}


/* Resolve IN_FENCE_FD descriptors (in-process sync files) to render fences. */
static int resolve_in_fences(drmphx_atomic_t *a)
{
	v3da_fence_t f;
	uint32_t i;
	int rc;

	for (i = 0u; i < a->nplanes; i++) {
		if (a->in_fence_fd[i] < 0) {
			continue;
		}
		rc = drmphx_syncfile_get(a->in_fence_fd[i], &f);
		if (rc != 0) {
			return -EINVAL;   /* not a sync file of this process (cross-process: server gap) */
		}
		memcpy(&a->st[i].in_fence, &f, sizeof(kms_fence_t));   /* identical layouts (kms_proto.h) */
	}
	return 0;
}


static int atomic_commit(drmphx_conn_t *c, drmphx_atomic_t *a, uint32_t flags, uint64_t user_data, int blocking)
{
	kms_atomic_req_t h;
	kms_resp_t r;
	uint32_t i, implicit;
	int rc;

	if (a->out_fence_ptr != 0u) {
		return -ENOSYS;   /* gap: a kms out-fence (sync file signalled at flip) */
	}
	rc = resolve_in_fences(a);
	if (rc != 0) {
		return rc;
	}
	implicit = 0u;
	for (i = 0u; (a->active != 0u) && (i < a->nplanes); i++) {
		if ((a->in_fence_fd[i] < 0) && (a->st[i].fb_id != 0u) && ((flags & KMS_ATOMIC_TEST_ONLY) == 0u) &&
				(implicit_attach(c, a->st[i].fb_id, &a->st[i].in_fence) != 0)) {
			implicit |= 1u << i;
		}
	}
	if ((a->active != 0u) && (a->nplanes == 0u)) {
		/* CRTC-only commit (e.g. MODE_ID/ACTIVE, or an event request): carry the
		 * primary unchanged so the server validates the mode and can send the event. */
		rc = plane_baseline(c, KMS_ID_PLANE(a->crtc_id - KMS_ID_CRTC(0u), 0u), &a->st[0]);
		if (rc != 0) {
			return rc;
		}
		if (a->st[0].fb_id == 0u) {
			return ((flags & KMS_PAGE_FLIP_EVENT) != 0u) ? -EINVAL : 0;   /* nothing shown, nothing to do */
		}
		a->nplanes = 1u;
	}

	memset(&h, 0, sizeof(h));
	h.flags = flags;
	h.nplanes = a->nplanes;
	h.user_data = user_data;
	h.crtc_id = a->crtc_id;
	h.mode_blob = a->mode_blob;
	h.active = a->active;
	{
		/* The per-frame path: send the plane states page-rounded so the window has no
		 * unaligned end (E5: +30 us each); the server parses by nplanes. */
		kms_req_t rq;
		size_t n = (size_t)a->nplanes * sizeof(kms_atomic_plane_t);
		memset(&rq, 0, sizeof(rq));
		rq.magic = KMS_MAGIC;
		rq.op = KMS_OP_ATOMIC;
		memcpy(&rq.u, &h, sizeof(h));
		rc = drmphx_call(c, &rq, &r, a->st, n, (n + (size_t)_PAGE_SIZE - 1u) & ~((size_t)_PAGE_SIZE - 1u), NULL, 0u);
		if ((rc == -ENODEV) && (implicit != 0u)) {
			for (i = 0u; i < a->nplanes; i++) {
				if ((implicit & (1u << i)) != 0u) {
					implicit_fallback(c, a->st[i].fb_id, &a->st[i].in_fence);
				}
			}
			implicit = 0u;
			rc = drmphx_call(c, &rq, &r, a->st, n, (n + (size_t)_PAGE_SIZE - 1u) & ~((size_t)_PAGE_SIZE - 1u), NULL,
				0u);
		}
	}
	if (rc != 0) {
		return rc;
	}
	if (implicit != 0u) {
		c->u.kms.implicit++;
	}
	if ((flags & KMS_ATOMIC_TEST_ONLY) != 0u) {
		return 0;
	}
	if (a->active == 0u) {
		mirror_invalidate(c);   /* the server turned every plane off */
	}
	else {
		mirror_store(c, a->st, a->nplanes);
	}
	if (blocking != 0) {
		wait_commit_done(c, a->crtc_id, r.u.flip.sequence);
	}
	return 0;
}


static int ioc_atomic(drmphx_conn_t *c, struct drm_mode_atomic *at)
{
	drmphx_atomic_t a;
	const uint32_t allowed = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_ATOMIC_TEST_ONLY |
		DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_ATOMIC_ALLOW_MODESET;
	int rc;

	if (((at->flags & ~allowed) != 0u) || (at->reserved != 0u)) {
		return -EINVAL;
	}
	if (((at->flags & DRM_MODE_ATOMIC_TEST_ONLY) != 0u) && ((at->flags & DRM_MODE_PAGE_FLIP_EVENT) != 0u)) {
		return -EINVAL;   /* DRM rule */
	}
	rc = drmphx_atomic_flatten((const uint32_t *)(uintptr_t)at->objs_ptr, (const uint32_t *)(uintptr_t)at->count_props_ptr,
		(const uint32_t *)(uintptr_t)at->props_ptr, (const uint64_t *)(uintptr_t)at->prop_values_ptr, at->count_objs,
		plane_baseline, c, &a);
	if (rc != 0) {
		return rc;
	}
	return atomic_commit(c, &a, at->flags, at->user_data,
		((at->flags & (DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_ATOMIC_TEST_ONLY)) == 0u) ? 1 : 0);
}


/* Legacy property set = a one-property atomic commit (blocking). */
static int set_one_prop(drmphx_conn_t *c, uint32_t obj, uint32_t prop, uint64_t value)
{
	drmphx_atomic_t a;
	uint32_t one = 1u;
	int rc;

	rc = drmphx_atomic_flatten(&obj, &one, &prop, &value, 1u, plane_baseline, c, &a);
	if (rc != 0) {
		return rc;
	}
	if ((a.nplanes == 0u) && (a.active != 0u)) {
		return 0;   /* connector DPMS / link-status, CRTC MODE_ID of the current mode: nothing to apply */
	}
	return atomic_commit(c, &a, 0u, 0u, 1);
}


static int ioc_setplane(drmphx_conn_t *c, const struct drm_mode_set_plane *sp)
{
	drmphx_atomic_t a;
	int rc;

	memset(&a, 0, sizeof(a));
	a.active = 1u;
	a.in_fence_fd[0] = -1;
	rc = plane_baseline(c, sp->plane_id, &a.st[0]);
	if (rc != 0) {
		return rc;
	}
	a.nplanes = 1u;
	a.st[0].plane_id = sp->plane_id;
	a.st[0].fb_id = sp->fb_id;
	a.st[0].crtc_id = (sp->fb_id != 0u) ? sp->crtc_id : 0u;
	a.st[0].crtc_x = sp->crtc_x;
	a.st[0].crtc_y = sp->crtc_y;
	a.st[0].crtc_w = sp->crtc_w;
	a.st[0].crtc_h = sp->crtc_h;
	a.st[0].src_x = sp->src_x;
	a.st[0].src_y = sp->src_y;
	a.st[0].src_w = sp->src_w;
	a.st[0].src_h = sp->src_h;
	a.crtc_id = (sp->crtc_id != 0u) ? sp->crtc_id : KMS_ID_CRTC(0u);
	return atomic_commit(c, &a, 0u, 0u, 1);
}


static int ioc_setcrtc(drmphx_conn_t *c, const struct drm_mode_crtc *sc)
{
	kms_set_crtc_req_t q;
	kms_obj_req_t oq;
	kms_resp_t r;
	uint64_t seq;
	int rc;

	memset(&q, 0, sizeof(q));
	q.crtc_id = sc->crtc_id;
	q.fb_id = sc->fb_id;
	q.x = (int32_t)sc->x;
	q.y = (int32_t)sc->y;
	if ((sc->count_connectors != 0u) && (sc->set_connectors_ptr != 0u)) {
		q.conn_id = ((const uint32_t *)(uintptr_t)sc->set_connectors_ptr)[0];
	}
	if (sc->mode_valid != 0u) {
		q.mode_hdisplay = sc->mode.hdisplay;
		q.mode_vdisplay = sc->mode.vdisplay;
	}
	memset(&oq, 0, sizeof(oq));
	oq.id = sc->crtc_id;
	rc = kcall(c, KMS_OP_GET_CRTC, &oq, sizeof(oq), &r, NULL, 0u, NULL, 0u);
	if (rc != 0) {
		return rc;
	}
	seq = r.u.crtc.sequence;
	if (q.fb_id == 0xffffffffu) {
		q.fb_id = r.u.crtc.fb_id;   /* DRM: -1 = keep the current framebuffer */
	}
	if ((sc->mode_valid == 0u) && (q.fb_id != 0u)) {
		return -EINVAL;   /* DRM: a framebuffer needs a mode */
	}
	rc = kcall(c, KMS_OP_SET_CRTC, &q, sizeof(q), &r, NULL, 0u, NULL, 0u);
	mirror_invalidate(c);
	if (rc != 0) {
		return rc;
	}
	wait_commit_done(c, sc->crtc_id, seq);
	return 0;
}


/* ========================================================================= */
/* vblank                                                                     */
/* ========================================================================= */

static int crtc_seq(drmphx_conn_t *c, uint32_t crtc_id, uint64_t *seq, int64_t *ns)
{
	kms_vblank_req_t v;
	kms_resp_t r;
	int rc;

	memset(&v, 0, sizeof(v));
	v.crtc_id = crtc_id;
	rc = kcall(c, KMS_OP_CRTC_GET_SEQUENCE, &v, sizeof(v), &r, NULL, 0u, NULL, 0u);
	if (rc == 0) {
		*seq = r.u.vblank.sequence;
		if (ns != NULL) {
			*ns = r.u.vblank.time_ns;
		}
	}
	return rc;
}


/* DRM_IOCTL_WAIT_VBLANK. DRM encodes _DRM_VBLANK_RELATIVE = 1, ABSOLUTE = 0; the
 * server's KMS_VBL_* tests bit 0 as ABSOLUTE (kms_proto.h labels them the other
 * way round - the server code is what counts). This function always sends an
 * absolute 64-bit target it computed itself, so the encodings cannot mix. */
static int ioc_wait_vblank(drmphx_conn_t *c, union drm_wait_vblank *wv)
{
	uint32_t type = wv->request.type, idx, i;
	uint64_t cur, target;
	kms_vblank_req_t v;
	kms_resp_t r;
	int rc;

	if ((type & (_DRM_VBLANK_SIGNAL | _DRM_VBLANK_FLIP)) != 0u) {
		return -EINVAL;
	}
	idx = ((type & _DRM_VBLANK_SECONDARY) != 0u) ? 1u : ((type & _DRM_VBLANK_HIGH_CRTC_MASK) >> _DRM_VBLANK_HIGH_CRTC_SHIFT);
	if (idx >= KMS_MAX_CRTCS) {
		return -EINVAL;
	}
	rc = crtc_seq(c, KMS_ID_CRTC(idx), &cur, NULL);
	if (rc != 0) {
		return rc;
	}
	if ((type & _DRM_VBLANK_RELATIVE) != 0u) {
		target = cur + wv->request.sequence;
	}
	else {
		/* 32-bit absolute sequence -> the 64-bit counter nearest to it */
		target = (cur & ~0xffffffffull) | wv->request.sequence;
		if ((target + 0x80000000ull) < cur) {
			target += 0x100000000ull;
		}
		else if (target > cur + 0x80000000ull) {
			target -= 0x100000000ull;
		}
	}
	if (((type & _DRM_VBLANK_NEXTONMISS) != 0u) && (target <= cur)) {
		target = cur + 1u;
	}

	memset(&v, 0, sizeof(v));
	v.crtc_id = KMS_ID_CRTC(idx);
	v.type = KMS_VBL_ABSOLUTE | (((type & _DRM_VBLANK_EVENT) != 0u) ? KMS_VBL_EVENT : 0u);
	v.sequence = target;
	v.user_data = wv->request.signal;
	for (i = 0u; i < 64u; i++) {   /* the server parks a blocking wait <= KMS_READ_MAX_MS */
		rc = kcall(c, KMS_OP_WAIT_VBLANK, &v, sizeof(v), &r, NULL, 0u, NULL, 0u);
		if (rc != -EAGAIN) {
			break;
		}
	}
	if (rc != 0) {
		return rc;
	}
	wv->reply.type = type;
	wv->reply.sequence = (uint32_t)r.u.vblank.sequence;
	wv->reply.tval_sec = (long)(r.u.vblank.time_ns / 1000000000LL);
	wv->reply.tval_usec = (long)((r.u.vblank.time_ns % 1000000000LL) / 1000LL);
	return 0;
}


/* ========================================================================= */
/* PRIME                                                                      */
/* ========================================================================= */

static int ioc_prime_export(drmphx_conn_t *c, struct drm_prime_handle *ph)
{
	char path[48];
	kms_handle_req_t q;
	kms_resp_t r;
	int rc, bfd;

	memset(&q, 0, sizeof(q));
	q.handle = ph->handle;
	rc = kcall(c, KMS_OP_PRIME_EXPORT, &q, sizeof(q), &r, NULL, 0u, NULL, 0u);
	if (rc != 0) {
		return rc;
	}
	if (r.u.dumb.mem.kind != KMS_MEM_OID) {
		return -ENOSYS;   /* pan-backend firmware-fb slots are MAP_PHYSMEM: memExport refuses them */
	}
	/* A G7 import answers its exporter's name: re-exporting it reopens that (as G4a
	 * does on the render node) - the same pages, zero-copy. */
	(void)snprintf(path, sizeof(path), "%s/%llu", (r.u.dumb.mem.port == c->u.kms.buf_port) ? KMS_BUF_NS : V3DA_BUF_NS,
		(unsigned long long)r.u.dumb.mem.addr);
	bfd = open(path, O_RDONLY | (((ph->flags & DRM_CLOEXEC) != 0u) ? O_CLOEXEC : 0));
	if (bfd < 0) {
		return -errno;
	}
	dumb_store(c, &r.u.dumb);
	drmphx_prime_fd_note(bfd, &r.u.dumb.mem, DRMPHX_SRV_KMS, ph->handle);
	ph->fd = bfd;
	return 0;
}


/* PRIME_FD_TO_HANDLE on card0. One of this client's own /kmsbuf exports is the
 * original handle (DRM). A foreign buffer - a render-node BO exported as
 * "/v3dbuf/<id>" (G4) - is KMS_OP_PRIME_IMPORT (G7): the server opens and maps the
 * name itself and holds it while any framebuffer of it may be on screen; the reply
 * is a dumb-style handle whose memref is the exporter's name, so MAP_DUMB, ADDFB2,
 * the G13 implicit flip fence (fb_export) and a re-export all work on it. */
static int ioc_prime_import(drmphx_conn_t *c, struct drm_prime_handle *ph)
{
	kms_prime_import_req_t q;
	kms_memref_t m;
	kms_memref_t own;
	kms_resp_t r;
	int rc;

	rc = drmphx_prime_fd_lookup(ph->fd, &m);
	if (rc != 0) {
		return (rc == -EBADF) ? -EBADF : -EINVAL;
	}
	/* One of this client's own exports: DRM returns the original handle. */
	if ((m.port == c->u.kms.buf_port) && (m.addr <= 0xffffffffu) &&
			(drmphx_kms_token_memref(c, (uint32_t)m.addr, &own) == 0)) {
		ph->handle = (uint32_t)m.addr;
		return 0;
	}
	if (c->u.kms.hello.proto < KMS_PROTO_PRIME_IMPORT) {
		return -ENOSYS;   /* a proto-1 rpi4-kms (before G7) imports nothing foreign */
	}
	memset(&q, 0, sizeof(q));
	q.port = m.port;
	q.cache = m.cache;
	q.id = m.addr;
	q.size = m.size;   /* 0 = unknown here (another process exported it): the server sizes it (G3) */
	q.ns = (m.port == c->u.kms.buf_port) ? KMS_IMPORT_NS_KMSBUF : KMS_IMPORT_NS_V3DBUF;
	rc = kcall(c, KMS_OP_PRIME_IMPORT, &q, sizeof(q), &r, NULL, 0u, NULL, 0u);
	if (rc != 0) {
		return rc;
	}
	dumb_store(c, &r.u.dumb);
	ph->handle = r.u.dumb.handle;
	return 0;
}


/* ========================================================================= */
/* Dispatcher                                                                 */
/* ========================================================================= */

static int ioc_get_resources(drmphx_conn_t *c, struct drm_mode_card_res *res)
{
	uint32_t fbs[KMS_MAX_ARRAY], max = (res->count_fbs < KMS_MAX_ARRAY) ? res->count_fbs : KMS_MAX_ARRAY, i, n;
	kms_obj_req_t q;
	kms_resp_t r;
	int rc;

	memset(&q, 0, sizeof(q));
	q.max = (res->fb_id_ptr != 0u) ? max : 0u;
	rc = kcall(c, KMS_OP_GET_RESOURCES, &q, sizeof(q), &r, NULL, 0u, (q.max != 0u) ? fbs : NULL,
		(size_t)q.max * sizeof(uint32_t));
	if (rc != 0) {
		return rc;
	}
	n = (r.u.res.nfb < q.max) ? r.u.res.nfb : q.max;
	for (i = 0u; i < n; i++) {
		((uint32_t *)(uintptr_t)res->fb_id_ptr)[i] = fbs[i];
	}
	for (i = 0u; (i < r.u.res.ncrtc) && (i < res->count_crtcs) && (i < KMS_MAX_CRTCS) && (res->crtc_id_ptr != 0u); i++) {
		((uint32_t *)(uintptr_t)res->crtc_id_ptr)[i] = r.u.res.crtc[i];
	}
	for (i = 0u; (i < r.u.res.nconn) && (i < res->count_connectors) && (i < KMS_MAX_CRTCS) &&
			(res->connector_id_ptr != 0u); i++) {
		((uint32_t *)(uintptr_t)res->connector_id_ptr)[i] = r.u.res.conn[i];
	}
	for (i = 0u; (i < r.u.res.nenc) && (i < res->count_encoders) && (i < KMS_MAX_CRTCS) &&
			(res->encoder_id_ptr != 0u); i++) {
		((uint32_t *)(uintptr_t)res->encoder_id_ptr)[i] = r.u.res.enc[i];
	}
	res->count_fbs = r.u.res.nfb;
	res->count_crtcs = r.u.res.ncrtc;
	res->count_connectors = r.u.res.nconn;
	res->count_encoders = r.u.res.nenc;
	res->min_width = r.u.res.min_w;
	res->max_width = r.u.res.max_w;
	res->min_height = r.u.res.min_h;
	res->max_height = r.u.res.max_h;
	return 0;
}


static int obj_props(drmphx_conn_t *c, uint32_t obj, uint32_t type, uint64_t props_ptr, uint64_t values_ptr,
	uint32_t *count)
{
	kms_prop_value_t pv[KMS_MAX_ARRAY];
	kms_obj_req_t q;
	kms_resp_t r;
	uint32_t i, n, cap = *count;
	int rc;

	memset(&q, 0, sizeof(q));
	q.id = obj;
	q.type = type;
	q.max = ((cap != 0u) && (props_ptr != 0u)) ? ((cap < KMS_MAX_ARRAY) ? cap : KMS_MAX_ARRAY) : 0u;
	rc = kcall(c, KMS_OP_GET_PROPERTIES, &q, sizeof(q), &r, NULL, 0u, (q.max != 0u) ? pv : NULL,
		(size_t)q.max * sizeof(kms_prop_value_t));
	if (rc != 0) {
		return rc;
	}
	n = (r.u.count < q.max) ? r.u.count : q.max;
	for (i = 0u; i < n; i++) {
		((uint32_t *)(uintptr_t)props_ptr)[i] = pv[i].prop_id;
		if (values_ptr != 0u) {
			((uint64_t *)(uintptr_t)values_ptr)[i] = pv[i].value;
		}
	}
	*count = r.u.count;
	return 0;
}


static int ioc_get_connector(drmphx_conn_t *c, struct drm_mode_get_connector *gc)
{
	kms_modeinfo_t modes[16];
	kms_obj_req_t q;
	kms_resp_t r;
	uint32_t max = (gc->count_modes < 16u) ? gc->count_modes : 16u, n, np;
	int rc;

	memset(&q, 0, sizeof(q));
	q.id = gc->connector_id;
	q.max = (gc->modes_ptr != 0u) ? max : 0u;
	rc = kcall(c, KMS_OP_GET_CONNECTOR, &q, sizeof(q), &r, NULL, 0u, (q.max != 0u) ? modes : NULL,
		(size_t)q.max * sizeof(kms_modeinfo_t));
	if (rc != 0) {
		return rc;
	}
	n = (r.u.conn.nmodes < q.max) ? r.u.conn.nmodes : q.max;
	if (n != 0u) {
		memcpy((void *)(uintptr_t)gc->modes_ptr, modes, (size_t)n * sizeof(kms_modeinfo_t));   /* == drm_mode_modeinfo */
	}
	if ((gc->count_encoders >= 1u) && (gc->encoders_ptr != 0u) && (r.u.conn.encoder_id != 0u)) {
		((uint32_t *)(uintptr_t)gc->encoders_ptr)[0] = r.u.conn.encoder_id;
	}
	np = gc->count_props;
	if ((np != 0u) && (gc->props_ptr != 0u)) {
		rc = obj_props(c, gc->connector_id, KMS_OBJ_CONNECTOR, gc->props_ptr, gc->prop_values_ptr, &np);
		if (rc != 0) {
			return rc;
		}
	}
	gc->count_modes = r.u.conn.nmodes;
	gc->count_encoders = (r.u.conn.encoder_id != 0u) ? 1u : 0u;
	gc->count_props = r.u.conn.nprops;
	gc->encoder_id = r.u.conn.encoder_id;
	gc->connector_type = r.u.conn.type;
	gc->connector_type_id = r.u.conn.type_id;
	gc->connection = r.u.conn.connection;
	gc->mm_width = r.u.conn.mm_width;
	gc->mm_height = r.u.conn.mm_height;
	gc->subpixel = r.u.conn.subpixel;
	return 0;
}


static int ioc_get_crtc(drmphx_conn_t *c, struct drm_mode_crtc *gc)
{
	kms_modeinfo_t mode;
	kms_obj_req_t q;
	kms_resp_t r;
	int rc;

	memset(&q, 0, sizeof(q));
	memset(&mode, 0, sizeof(mode));
	q.id = gc->crtc_id;
	rc = kcall(c, KMS_OP_GET_CRTC, &q, sizeof(q), &r, NULL, 0u, &mode, sizeof(mode));
	if (rc != 0) {
		return rc;
	}
	gc->fb_id = r.u.crtc.fb_id;
	gc->x = r.u.crtc.x;
	gc->y = r.u.crtc.y;
	gc->gamma_size = r.u.crtc.gamma_size;
	gc->mode_valid = r.u.crtc.mode_valid;
	memcpy(&gc->mode, &mode, sizeof(gc->mode));
	return 0;
}


static int ioc_get_plane(drmphx_conn_t *c, struct drm_mode_get_plane *gp)
{
	kms_obj_req_t q;
	kms_resp_t r;
	uint32_t i;
	int rc;

	memset(&q, 0, sizeof(q));
	q.id = gp->plane_id;
	rc = kcall(c, KMS_OP_GET_PLANE, &q, sizeof(q), &r, NULL, 0u, NULL, 0u);
	if (rc != 0) {
		return rc;
	}
	for (i = 0u; (i < r.u.plane.nformats) && (i < KMS_PLANE_MAX_FMTS) && (i < gp->count_format_types) &&
			(gp->format_type_ptr != 0u); i++) {
		((uint32_t *)(uintptr_t)gp->format_type_ptr)[i] = r.u.plane.formats[i];
	}
	gp->crtc_id = r.u.plane.crtc_id;
	gp->fb_id = r.u.plane.fb_id;
	gp->possible_crtcs = r.u.plane.possible_crtcs;
	gp->gamma_size = 0u;
	gp->count_format_types = r.u.plane.nformats;
	return 0;
}


static int ioc_get_property(drmphx_conn_t *c, struct drm_mode_get_property *gp)
{
	uint8_t data[KMS_MAX_ENUMS * sizeof(kms_prop_enum_t)];
	kms_obj_req_t q;
	kms_resp_t r;
	int rc;

	memset(&q, 0, sizeof(q));
	memset(data, 0, sizeof(data));
	q.id = gp->prop_id;
	q.max = KMS_MAX_ENUMS;
	rc = kcall(c, KMS_OP_GET_PROPERTY, &q, sizeof(q), &r, NULL, 0u, data, sizeof(data));
	if (rc != 0) {
		return rc;
	}
	drmphx_fill_property(&r.u.prop, data, sizeof(data), gp);
	return 0;
}


static int ioc_addfb2(drmphx_conn_t *c, struct drm_mode_fb_cmd2 *f)
{
	kms_addfb2_req_t q;
	kms_memref_t mem;
	kms_resp_t r;
	int rc;

	if ((f->flags & ~(uint32_t)DRM_MODE_FB_MODIFIERS) != 0u) {
		return -EINVAL;   /* interlaced: no */
	}
	if ((f->handles[1] != 0u) || (f->handles[2] != 0u) || (f->handles[3] != 0u)) {
		return -EINVAL;   /* Stage A: single-plane formats only */
	}
	if (((f->flags & DRM_MODE_FB_MODIFIERS) != 0u) && (f->modifier[0] != DRM_FORMAT_MOD_LINEAR)) {
		return -EINVAL;
	}
	memset(&q, 0, sizeof(q));
	q.width = f->width;
	q.height = f->height;
	q.format = f->pixel_format;
	q.handle = f->handles[0];
	q.pitch = f->pitches[0];
	q.offset = f->offsets[0];
	q.modifier = KMS_MOD_LINEAR;
	rc = kcall(c, KMS_OP_ADDFB2, &q, sizeof(q), &r, NULL, 0u, NULL, 0u);
	if (rc == 0) {
		f->fb_id = r.u.fb.fb_id;
		/* the handle's buffer while the handle is certainly open (the dumb cache: no IPC
		 * for this client's own and imported buffers) */
		if (drmphx_kms_token_memref(c, q.handle, &mem) != 0) {
			memset(&mem, 0, sizeof(mem));   /* KMS_MEM_NONE: no implicit sync for it */
		}
		fb_note(c, f->fb_id, q.handle, &mem);
	}
	return rc;
}


static int ioc_addfb(drmphx_conn_t *c, struct drm_mode_fb_cmd *f)
{
	struct drm_mode_fb_cmd2 f2;
	int rc;

	memset(&f2, 0, sizeof(f2));
	if ((f->bpp == 32u) && (f->depth == 24u)) {
		f2.pixel_format = DRM_FORMAT_XRGB8888;
	}
	else if ((f->bpp == 32u) && (f->depth == 32u)) {
		f2.pixel_format = DRM_FORMAT_ARGB8888;
	}
	else {
		return -EINVAL;   /* the planes scan 32 bpp only */
	}
	f2.width = f->width;
	f2.height = f->height;
	f2.handles[0] = f->handle;
	f2.pitches[0] = f->pitch;
	rc = ioc_addfb2(c, &f2);
	if (rc == 0) {
		f->fb_id = f2.fb_id;
	}
	return rc;
}


static int ioc_create_dumb(drmphx_conn_t *c, struct drm_mode_create_dumb *d)
{
	kms_create_dumb_req_t q;
	kms_resp_t r;
	int rc;

	if (d->flags != 0u) {
		return -EINVAL;   /* DRM: no flags defined */
	}
	memset(&q, 0, sizeof(q));
	q.width = d->width;
	q.height = d->height;
	q.bpp = d->bpp;
	rc = kcall(c, KMS_OP_CREATE_DUMB, &q, sizeof(q), &r, NULL, 0u, NULL, 0u);
	if (rc != 0) {
		return rc;
	}
	dumb_store(c, &r.u.dumb);
	d->handle = r.u.dumb.handle;
	d->pitch = r.u.dumb.pitch;
	d->size = r.u.dumb.size;
	return 0;
}


static int ioc_destroy_dumb(drmphx_conn_t *c, uint32_t handle)
{
	kms_handle_req_t q;

	memset(&q, 0, sizeof(q));
	q.handle = handle;
	dumb_drop(c, handle);
	return kcall(c, KMS_OP_DESTROY_DUMB, &q, sizeof(q), NULL, NULL, 0u, NULL, 0u);
}


int drmphx_kms_ioctl(drmphx_conn_t *c, int fd, unsigned nr, void *arg)
{
	kms_cap_req_t cq;
	kms_resp_t r;
	int rc;

	(void)fd;
	if (c->srv != DRMPHX_SRV_KMS) {
		return -EINVAL;
	}
	switch (nr) {
		case NR(DRM_IOCTL_GET_CAP): {
			struct drm_get_cap *g = arg;
			memset(&cq, 0, sizeof(cq));
			cq.cap = g->capability;
			rc = kcall(c, KMS_OP_GET_CAP, &cq, sizeof(cq), &r, NULL, 0u, NULL, 0u);
			if (rc == 0) {
				g->value = r.u.cap.value;
			}
			return rc;
		}
		case NR(DRM_IOCTL_SET_CLIENT_CAP): {
			const struct drm_set_client_cap *s = arg;
			memset(&cq, 0, sizeof(cq));
			cq.cap = s->capability;
			cq.value = s->value;
			rc = kcall(c, KMS_OP_SET_CLIENT_CAP, &cq, sizeof(cq), NULL, NULL, 0u, NULL, 0u);
			if ((rc == 0) && (s->capability == DRM_CLIENT_CAP_ATOMIC)) {
				/* DRM (drm_setclientcap): ATOMIC sets universal_planes to the same value.
				 * Atomic-only clients (Mesa's VK_KHR_display WSI) never set
				 * UNIVERSAL_PLANES themselves, and rpi4-kms lists the primary plane only
				 * with it (M5). (DRM also sets aspect_ratio_allowed; rpi4-kms stores that
				 * cap but uses it nowhere, so it is not mirrored.) */
				cq.cap = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
				rc = kcall(c, KMS_OP_SET_CLIENT_CAP, &cq, sizeof(cq), NULL, NULL, 0u, NULL, 0u);
			}
			return rc;
		}
		case NR(DRM_IOCTL_SET_MASTER):
			return kcall(c, KMS_OP_SET_MASTER, NULL, 0u, NULL, NULL, 0u, NULL, 0u);
		case NR(DRM_IOCTL_DROP_MASTER):
			return kcall(c, KMS_OP_DROP_MASTER, NULL, 0u, NULL, NULL, 0u, NULL, 0u);
		case NR(DRM_IOCTL_AUTH_MAGIC):
			return kcall(c, KMS_OP_AUTH_MAGIC, NULL, 0u, NULL, NULL, 0u, NULL, 0u);

		case NR(DRM_IOCTL_GEM_CLOSE):
			return ioc_destroy_dumb(c, ((const struct drm_gem_close *)arg)->handle);
		case NR(DRM_IOCTL_PRIME_HANDLE_TO_FD):
			return ioc_prime_export(c, arg);
		case NR(DRM_IOCTL_PRIME_FD_TO_HANDLE):
			return ioc_prime_import(c, arg);
		case NR(DRM_IOCTL_GEM_FLINK):
		case NR(DRM_IOCTL_GEM_OPEN):
			return -ENOSYS;   /* global GEM names: not provided (PRIME is the sharing path) */

		case NR(DRM_IOCTL_MODE_GETRESOURCES):
			return ioc_get_resources(c, arg);
		case NR(DRM_IOCTL_MODE_GETCONNECTOR):
			return ioc_get_connector(c, arg);
		case NR(DRM_IOCTL_MODE_GETENCODER): {
			struct drm_mode_get_encoder *e = arg;
			kms_obj_req_t q;
			memset(&q, 0, sizeof(q));
			q.id = e->encoder_id;
			rc = kcall(c, KMS_OP_GET_ENCODER, &q, sizeof(q), &r, NULL, 0u, NULL, 0u);
			if (rc == 0) {
				e->encoder_type = r.u.enc.type;
				e->crtc_id = r.u.enc.crtc_id;
				e->possible_crtcs = r.u.enc.possible_crtcs;
				e->possible_clones = r.u.enc.possible_clones;
			}
			return rc;
		}
		case NR(DRM_IOCTL_MODE_GETCRTC):
			return ioc_get_crtc(c, arg);
		case NR(DRM_IOCTL_MODE_SETCRTC):
			return ioc_setcrtc(c, arg);
		case NR(DRM_IOCTL_MODE_GETPLANERESOURCES): {
			struct drm_mode_get_plane_res *pr = arg;
			uint32_t i;
			rc = kcall(c, KMS_OP_GET_PLANE_RESOURCES, NULL, 0u, &r, NULL, 0u, NULL, 0u);
			if (rc != 0) {
				return rc;
			}
			for (i = 0u; (i < r.u.plane_res.nplanes) && (i < 13u) && (i < pr->count_planes) && (pr->plane_id_ptr != 0u); i++) {
				((uint32_t *)(uintptr_t)pr->plane_id_ptr)[i] = r.u.plane_res.plane[i];
			}
			pr->count_planes = r.u.plane_res.nplanes;
			return 0;
		}
		case NR(DRM_IOCTL_MODE_GETPLANE):
			return ioc_get_plane(c, arg);
		case NR(DRM_IOCTL_MODE_SETPLANE):
			return ioc_setplane(c, arg);
		case NR(DRM_IOCTL_MODE_OBJ_GETPROPERTIES): {
			struct drm_mode_obj_get_properties *op = arg;
			return obj_props(c, op->obj_id, op->obj_type, op->props_ptr, op->prop_values_ptr, &op->count_props);
		}
		case NR(DRM_IOCTL_MODE_GETPROPERTY):
			return ioc_get_property(c, arg);
		case NR(DRM_IOCTL_MODE_OBJ_SETPROPERTY): {
			const struct drm_mode_obj_set_property *sp = arg;
			return set_one_prop(c, sp->obj_id, sp->prop_id, sp->value);
		}
		case NR(DRM_IOCTL_MODE_SETPROPERTY): {
			const struct drm_mode_connector_set_property *sp = arg;
			return set_one_prop(c, sp->connector_id, sp->prop_id, sp->value);
		}
		case NR(DRM_IOCTL_MODE_GETPROPBLOB): {
			struct drm_mode_get_blob *gb = arg;
			kms_blob_t q;
			memset(&q, 0, sizeof(q));
			q.id = gb->blob_id;
			rc = kcall(c, KMS_OP_GET_BLOB, &q, sizeof(q), &r, NULL, 0u,
				((gb->length != 0u) && (gb->data != 0u)) ? (void *)(uintptr_t)gb->data : NULL,
				((gb->length != 0u) && (gb->data != 0u)) ? gb->length : 0u);
			if (rc == 0) {
				gb->length = r.u.blob.length;
			}
			return rc;
		}
		case NR(DRM_IOCTL_MODE_CREATEPROPBLOB): {
			struct drm_mode_create_blob *cb = arg;
			if ((cb->length == 0u) || (cb->data == 0u)) {
				return -EINVAL;
			}
			rc = kcall(c, KMS_OP_CREATE_BLOB, NULL, 0u, &r, (const void *)(uintptr_t)cb->data, cb->length, NULL, 0u);
			if (rc == 0) {
				cb->blob_id = r.u.blob.id;
			}
			return rc;
		}
		case NR(DRM_IOCTL_MODE_DESTROYPROPBLOB): {
			kms_blob_t q;
			memset(&q, 0, sizeof(q));
			q.id = ((const struct drm_mode_destroy_blob *)arg)->blob_id;
			return kcall(c, KMS_OP_DESTROY_BLOB, &q, sizeof(q), NULL, NULL, 0u, NULL, 0u);
		}

		case NR(DRM_IOCTL_MODE_CREATE_DUMB):
			return ioc_create_dumb(c, arg);
		case NR(DRM_IOCTL_MODE_MAP_DUMB): {
			struct drm_mode_map_dumb *m = arg;
			kms_memref_t mem;
			rc = drmphx_kms_token_memref(c, m->handle, &mem);
			if (rc == 0) {
				m->offset = DRMPHX_TOKEN(m->handle);   /* for drmPhoenixMmap(), not mmap() */
			}
			return rc;
		}
		case NR(DRM_IOCTL_MODE_DESTROY_DUMB):
			return ioc_destroy_dumb(c, ((const struct drm_mode_destroy_dumb *)arg)->handle);
		case NR(DRM_IOCTL_MODE_ADDFB):
			return ioc_addfb(c, arg);
		case NR(DRM_IOCTL_MODE_ADDFB2):
			return ioc_addfb2(c, arg);
		case NR(DRM_IOCTL_MODE_RMFB):
		case NR(DRM_IOCTL_MODE_CLOSEFB): {
			kms_fb_resp_t q;
			memset(&q, 0, sizeof(q));
			q.fb_id = *(const unsigned int *)arg;   /* CLOSEFB's struct starts with fb_id */
			rc = kcall(c, KMS_OP_RMFB, &q, sizeof(q), NULL, NULL, 0u, NULL, 0u);
			mirror_invalidate(c);
			if (rc == 0) {
				fb_drop(c, q.fb_id);
			}
			return rc;
		}
		case NR(DRM_IOCTL_MODE_DIRTYFB):
			return 0;   /* the planes scan memory directly */
		case NR(DRM_IOCTL_MODE_PAGE_FLIP): {
			const struct drm_mode_crtc_page_flip_target *pf = arg;
			kms_page_flip_req_t q;
			int attached;
			if ((pf->flags & ~(uint32_t)(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC)) != 0u) {
				return -EINVAL;   /* DRM_CAP_PAGE_FLIP_TARGET = 0 */
			}
			memset(&q, 0, sizeof(q));
			q.crtc_id = pf->crtc_id;
			q.fb_id = pf->fb_id;
			q.flags = pf->flags;
			q.user_data = pf->user_data;
			attached = implicit_attach(c, q.fb_id, &q.in_fence);   /* G13 */
			rc = kcall(c, KMS_OP_PAGE_FLIP, &q, sizeof(q), &r, NULL, 0u, NULL, 0u);
			if ((rc == -ENODEV) && (attached != 0)) {
				implicit_fallback(c, q.fb_id, &q.in_fence);
				attached = 0;
				rc = kcall(c, KMS_OP_PAGE_FLIP, &q, sizeof(q), &r, NULL, 0u, NULL, 0u);
			}
			if ((rc == 0) && (attached != 0)) {
				c->u.kms.implicit++;
			}
			mirror_invalidate(c);
			return rc;
		}
		case NR(DRM_IOCTL_MODE_ATOMIC):
			return ioc_atomic(c, arg);

		case NR(DRM_IOCTL_WAIT_VBLANK):
			return ioc_wait_vblank(c, arg);
		case NR(DRM_IOCTL_CRTC_GET_SEQUENCE): {
			struct drm_crtc_get_sequence *gs = arg;
			uint64_t seq;
			int64_t ns;
			rc = crtc_seq(c, gs->crtc_id, &seq, &ns);
			if (rc == 0) {
				gs->active = 1u;
				gs->sequence = seq;
				gs->sequence_ns = ns;
			}
			return rc;
		}
		case NR(DRM_IOCTL_CRTC_QUEUE_SEQUENCE): {
			struct drm_crtc_queue_sequence *qs = arg;
			kms_vblank_req_t v;
			if ((qs->flags & ~(uint32_t)(DRM_CRTC_SEQUENCE_RELATIVE | DRM_CRTC_SEQUENCE_NEXT_ON_MISS)) != 0u) {
				return -EINVAL;
			}
			memset(&v, 0, sizeof(v));
			v.crtc_id = qs->crtc_id;
			v.type = qs->flags;   /* same bits in the server (RELATIVE 1, NEXT_ON_MISS 2) */
			v.sequence = qs->sequence;
			v.user_data = qs->user_data;
			rc = kcall(c, KMS_OP_CRTC_QUEUE_SEQUENCE, &v, sizeof(v), &r, NULL, 0u, NULL, 0u);
			if (rc == 0) {
				qs->sequence = r.u.vblank.sequence;
			}
			return rc;
		}

		case NR(DRM_IOCTL_MODE_CURSOR):
		case NR(DRM_IOCTL_MODE_CURSOR2):
		case NR(DRM_IOCTL_MODE_GETGAMMA):
		case NR(DRM_IOCTL_MODE_SETGAMMA):
		case NR(DRM_IOCTL_MODE_GETFB):
		case NR(DRM_IOCTL_MODE_GETFB2):
			return -ENOSYS;   /* M3 follow-ups: legacy cursor -> cursor plane; gamma_size is 0 */

		case NR(DRM_IOCTL_SYNCOBJ_CREATE):
		case NR(DRM_IOCTL_SYNCOBJ_DESTROY):
		case NR(DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD):
		case NR(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE):
		case NR(DRM_IOCTL_SYNCOBJ_WAIT):
		case NR(DRM_IOCTL_SYNCOBJ_RESET):
		case NR(DRM_IOCTL_SYNCOBJ_SIGNAL):
		case NR(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT):
		case NR(DRM_IOCTL_SYNCOBJ_QUERY):
		case NR(DRM_IOCTL_SYNCOBJ_TRANSFER):
		case NR(DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL):
			return -EOPNOTSUPP;   /* DRM_CAP_SYNCOBJ = 0 on the display node (Linux: no DRIVER_SYNCOBJ) */

		default:
			return -EINVAL;   /* incl. the driver range (DRM_IOCTL_VC4_*): no 3D here */
	}
}
