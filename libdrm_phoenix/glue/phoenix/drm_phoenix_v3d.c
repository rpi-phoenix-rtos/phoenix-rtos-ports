/*
 * Phoenix-RTOS
 *
 * libdrm-phoenix - the render node (/dev/dri/renderD128, card1 -> rpi4-v3d-async)
 *
 * Marshals DRM_IOCTL_V3D_*, GEM_CLOSE, PRIME and DRM_IOCTL_SYNCOBJ_* into
 * rpi4-v3d-async requests (v3da_proto.h). The Mesa-facing semantics are the ones
 * the M1 adapter (tools/gpu-lane/v3d-async/v3da_winsys.c) established on the Pi,
 * moved behind drmIoctl() so upstream Mesa needs no Phoenix code for them:
 *   - WAIT_BO answers "busy" as -ETIME (Mesa's v3d_bo_wait decodes only that) and
 *     really waits (v3d_bo_map relies on it before every CPU access);
 *   - SYNCOBJ_WAIT takes an ABSOLUTE CLOCK_MONOTONIC deadline;
 *   - fast paths with no IPC: fences that already passed, read from the shared
 *     fence page; this client mirrors its own syncobjs' fences and every BO's
 *     last-use fence from its own submits;
 *   - sync files are emulated in-process (xf86drm_phoenix.c);
 *   - G6: a shared BO's fences are every client's (the server's last-use record):
 *     dma-buf sync-file ioctls and flips of foreign buffers ask the server for them.
 * Every parked server wait is bounded (V3DA_WAIT_MAX_MS): a parked Phoenix client
 * can be neither interrupted nor killed (E5), so longer waits loop here.
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
#include <sys/mman.h>

#include "drm_phoenix_priv.h"
#include "v3d_drm.h"


#define NR(r)              DRMPHX_IOC_NR(r)
#define V3D_NR(x)          (DRM_COMMAND_BASE + (x))
#define V3DA_SLOT_BITS     13u          /* == V3DA_HANDLE_SLOT_BITS (server, v3da.h) */
#define V3DA_SPIN_US       50u          /* fast-path spin before the first server wait */
#define FOREVER_NS         (-1LL)

static drmphx_v3d_bo_t *bo_get(drmphx_conn_t *c, uint32_t handle);
static void implicit_forget(const drmphx_conn_t *c, uint32_t handle, int all);


static int vcall(drmphx_conn_t *c, uint32_t op, const void *u, size_t usize, v3da_resp_t *r)
{
	v3da_req_t rq;
	v3da_resp_t tmp;

	memset(&rq, 0, sizeof(rq));
	rq.magic = V3DA_MAGIC;
	rq.op = op;
	if ((u != NULL) && (usize != 0u)) {
		memcpy(&rq.u, u, usize);
	}
	return drmphx_call(c, &rq, (r != NULL) ? r : &tmp, NULL, 0u, 0u, NULL, 0u);
}


/* ========================================================================= */
/* Connection                                                                 */
/* ========================================================================= */

int drmphx_v3d_hello(drmphx_conn_t *c, int fd)
{
	v3da_hello_t h;
	void *fp;
	int rc;

	memset(&h, 0, sizeof(h));
	h.proto = V3DA_PROTO_VERSION;
	rc = ioctl(fd, V3DA_IOC_HELLO, &h);
	if ((rc < 0) && (errno == EPROTO)) {
		/* A proto-2 server (before G4) accepts exactly 2, a proto-3 one (before G6) 2..3:
		 * speak 2. The reply's proto (the server's version) gates what this connection
		 * sends (BO_EXPORT from 3, BO_LAST_FENCE / BO_ATTACH_FENCE from 4). */
		memset(&h, 0, sizeof(h));
		h.proto = V3DA_PROTO_BASE;
		rc = ioctl(fd, V3DA_IOC_HELLO, &h);
	}
	if (rc < 0) {
		return (errno != 0) ? -errno : -EIO;
	}
	if (h.client_id == 0u) {
		return -EPROTO;
	}
	c->oid.id = h.client_id;
	c->u.v3d.hello = h;
	fp = drmphx_map_memref(h.fence_page.kind, h.fence_page.cache, h.fence_page.port, h.fence_page.size,
		h.fence_page.addr, (size_t)h.fence_page.size, PROT_READ, NULL, 0);
	if (fp == MAP_FAILED) {
		return -ENOMEM;
	}
	c->u.v3d.fp = fp;
	return 0;
}


void drmphx_v3d_release(drmphx_conn_t *c)
{
	uint32_t i;

	implicit_forget(c, 0u, 1);   /* G13: no import record may outlive its connection */

	if (c->u.v3d.fp != NULL) {
		(void)munmap((void *)c->u.v3d.fp, (size_t)c->u.v3d.hello.fence_page.size);
		c->u.v3d.fp = NULL;
	}
	for (i = 0u; i < DRMPHX_V3D_BO_CHUNKS; i++) {
		free(c->u.v3d.bo[i]);
		c->u.v3d.bo[i] = NULL;
	}
}


/* The server reassigned our fence-page slot: the descriptor number now belongs
 * to a new open of the node (a new client). One load, no IPC. */
int drmphx_v3d_stale(const drmphx_conn_t *c)
{
	const volatile v3da_fence_slot_t *s;

	if ((c->u.v3d.fp == NULL) || (c->u.v3d.hello.slot >= V3DA_FENCE_NSLOTS)) {
		return 0;
	}
	s = &c->u.v3d.fp->slot[c->u.v3d.hello.slot];
	return ((uint32_t)__atomic_load_n(&s->gen, __ATOMIC_ACQUIRE) != c->u.v3d.hello.slot_gen) ? 1 : 0;
}


/* ========================================================================= */
/* Fences                                                                     */
/* ========================================================================= */

static int fence_signaled(const drmphx_conn_t *c, const v3da_fence_t *f)
{
	const volatile v3da_fence_slot_t *s;

	if (f->seqno == 0u) {
		return 1;
	}
	if ((c->u.v3d.fp == NULL) || (f->slot >= V3DA_FENCE_NSLOTS) || (f->queue >= V3DA_Q_COUNT)) {
		return 0;
	}
	s = &c->u.v3d.fp->slot[f->slot];
	if ((uint32_t)__atomic_load_n(&s->gen, __ATOMIC_ACQUIRE) != f->gen) {
		return 1;   /* slot reassigned: every fence of the old owner completed */
	}
	return (__atomic_load_n(&s->completed[f->queue], __ATOMIC_ACQUIRE) >= f->seqno) ? 1 : 0;
}


/* Remaining budget of an absolute deadline (us) as one server wait's timeout. */
static uint32_t slice_ms(uint64_t deadline_us, int forever)
{
	uint64_t now = drmphx_now_us(), left;

	if (forever != 0) {
		return V3DA_WAIT_MAX_MS;
	}
	if (now >= deadline_us) {
		return 0u;
	}
	left = (deadline_us - now + 999u) / 1000u;
	return (left > V3DA_WAIT_MAX_MS) ? V3DA_WAIT_MAX_MS : (uint32_t)left;
}


/* rel_ns < 0: forever; 0: poll. -ETIMEDOUT on expiry. */
static int fence_wait(drmphx_conn_t *c, const v3da_fence_t *f, int64_t rel_ns)
{
	v3da_fence_wait_req_t w;
	uint64_t t0 = drmphx_now_us(), deadline;
	int forever = (rel_ns < 0) ? 1 : 0, rc;

	if (fence_signaled(c, f) != 0) {
		return 0;
	}
	if (rel_ns == 0) {
		return -ETIMEDOUT;
	}
	deadline = t0 + ((rel_ns > 0) ? (uint64_t)rel_ns / 1000u : 0u);
	do {
		if (fence_signaled(c, f) != 0) {
			return 0;
		}
	} while ((drmphx_now_us() - t0) < V3DA_SPIN_US);

	do {
		memset(&w, 0, sizeof(w));
		w.fence = *f;
		w.timeout_ms = slice_ms(deadline, forever);
		c->u.v3d.ipc_waits++;
		rc = vcall(c, V3DA_OP_FENCE_WAIT, &w, sizeof(w), NULL);
	} while ((rc == -ETIMEDOUT) && ((forever != 0) || (drmphx_now_us() < deadline)));
	return rc;
}


int drmphx_v3d_fence_signaled(const drmphx_conn_t *c, const v3da_fence_t *f)
{
	return fence_signaled(c, f);
}


int drmphx_v3d_fence_wait(drmphx_conn_t *c, const v3da_fence_t *f)
{
	return fence_wait(c, f, FOREVER_NS);
}


/* ========================================================================= */
/* Implicit sync for flips (G13, M3 part 2)                                   */
/* ========================================================================= */

/* Process-wide: which render BO a buffer export {namespace port, id} was imported
 * as. Entries die with their handle (GEM_CLOSE) or their connection, both under
 * IMP.lock, so a lookup that holds IMP.lock always reads a live connection.
 * Lock order: G.lock (connection teardown) -> IMP.lock -> conn->lock. */
#define DRMPHX_MAX_IMPLICIT 64u

static struct {
	pthread_mutex_t lock;
	struct {
		drmphx_conn_t *conn;     /* NULL = free */
		uint32_t port;
		uint64_t id;
		uint32_t handle;
	} e[DRMPHX_MAX_IMPLICIT];
} IMP = { .lock = PTHREAD_MUTEX_INITIALIZER };


static void implicit_note(drmphx_conn_t *c, uint32_t port, uint64_t id, uint32_t handle)
{
	uint32_t i, k = DRMPHX_MAX_IMPLICIT;

	(void)pthread_mutex_lock(&IMP.lock);
	for (i = 0u; i < DRMPHX_MAX_IMPLICIT; i++) {
		if ((IMP.e[i].conn == c) && (IMP.e[i].handle == handle)) {
			k = i;   /* re-import of the same buffer: same handle (DRM) */
			break;
		}
		if ((IMP.e[i].conn == NULL) && (k == DRMPHX_MAX_IMPLICIT)) {
			k = i;
		}
	}
	if (k < DRMPHX_MAX_IMPLICIT) {   /* table full: that buffer's flips go unsynchronised, as without G13 */
		IMP.e[k].conn = c;
		IMP.e[k].port = port;
		IMP.e[k].id = id;
		IMP.e[k].handle = handle;
	}
	(void)pthread_mutex_unlock(&IMP.lock);
}


static void implicit_forget(const drmphx_conn_t *c, uint32_t handle, int all)
{
	uint32_t i;

	(void)pthread_mutex_lock(&IMP.lock);
	for (i = 0u; i < DRMPHX_MAX_IMPLICIT; i++) {
		if ((IMP.e[i].conn == c) && ((all != 0) || (IMP.e[i].handle == handle))) {
			memset(&IMP.e[i], 0, sizeof(IMP.e[i]));
		}
	}
	(void)pthread_mutex_unlock(&IMP.lock);
}


/* The newest unsignalled last-use fence over every import of {port, id} (called
 * with IMP.lock held). 1 = *f is it, 0 = nothing pending. */
static int implicit_pending(uint32_t port, uint64_t id, v3da_fence_t *f, drmphx_conn_t **conn)
{
	const drmphx_v3d_bo_t *b;
	v3da_fence_t cand;
	uint32_t i;
	int found = 0;

	for (i = 0u; i < DRMPHX_MAX_IMPLICIT; i++) {
		drmphx_conn_t *c = IMP.e[i].conn;
		if ((c == NULL) || (IMP.e[i].port != port) || (IMP.e[i].id != id)) {
			continue;
		}
		(void)pthread_mutex_lock(&c->lock);
		b = bo_get(c, IMP.e[i].handle);
		cand = (b != NULL) ? b->last : (v3da_fence_t){ 0 };
		(void)pthread_mutex_unlock(&c->lock);
		if ((cand.seqno != 0u) && (fence_signaled(c, &cand) == 0)) {
			*f = cand;   /* one render connection writes a scan-out buffer in practice: take the last found */
			*conn = c;
			found = 1;
		}
	}
	return found;
}


int drmphx_v3d_implicit_fence(uint32_t port, uint64_t id, v3da_fence_t *f)
{
	drmphx_conn_t *c;
	int found;

	(void)pthread_mutex_lock(&IMP.lock);
	found = implicit_pending(port, id, f, &c);
	(void)pthread_mutex_unlock(&IMP.lock);
	return found;
}


int drmphx_v3d_implicit_wait(uint32_t port, uint64_t id)
{
	v3da_fence_t f;
	drmphx_conn_t *c;
	int rc = 0;

	/* Waits with IMP.lock held (the connection must outlive the wait); bounded
	 * slices, and only on the rpi4-kms-without--G fallback path. */
	(void)pthread_mutex_lock(&IMP.lock);
	while ((rc == 0) && (implicit_pending(port, id, &f, &c) != 0)) {
		rc = fence_wait(c, &f, FOREVER_NS);
	}
	(void)pthread_mutex_unlock(&IMP.lock);
	return rc;
}


/* ========================================================================= */
/* Cross-process implicit sync (G6)                                           */
/* ========================================================================= */

static void sync_req(v3da_bo_sync_req_t *q, uint32_t ns, uint32_t port, uint64_t id)
{
	memset(q, 0, sizeof(*q));
	q->ns = ns;
	q->port = port;
	q->id = id;
}


int drmphx_v3d_buffer_fences(drmphx_conn_t *c, uint32_t ns, uint32_t port, uint64_t id, v3da_fence_t *set,
	uint32_t max, uint32_t *n)
{
	v3da_bo_sync_req_t q;
	v3da_resp_t r;
	uint32_t i, round;
	int rc;

	*n = 0u;
	if (c->u.v3d.hello.proto < V3DA_PROTO_BO_SYNC) {
		return -ENOSYS;   /* a render server before G6 */
	}
	for (round = 0u; round < 16u; round++) {
		sync_req(&q, ns, port, id);
		rc = vcall(c, V3DA_OP_BO_LAST_FENCE, &q, sizeof(q), &r);
		if (rc == -ENOENT) {
			return 0;   /* the name is gone: nothing of it can still be pending for us */
		}
		if (rc != 0) {
			return rc;
		}
		if (((r.u.bo_fences.flags & V3DA_BO_FENCES_MORE) == 0u) || (round == 15u)) {
			for (i = 0u; (i < r.u.bo_fences.count) && (i < V3DA_BO_FENCES_MAX) && (*n < max); i++) {
				set[(*n)++] = r.u.bo_fences.f[i];
			}
			return 0;
		}
		/* More pending than one answer holds (four or more timelines on one buffer):
		 * wait for the newest ones and ask again. */
		for (i = 0u; (i < r.u.bo_fences.count) && (i < V3DA_BO_FENCES_MAX); i++) {
			rc = fence_wait(c, &r.u.bo_fences.f[i], FOREVER_NS);
			if (rc != 0) {
				return rc;
			}
		}
	}
	return 0;
}


int drmphx_v3d_buffer_attach(drmphx_conn_t *c, uint32_t ns, uint32_t port, uint64_t id, const v3da_fence_t *f,
	v3da_fence_t *join)
{
	v3da_bo_sync_req_t q;
	v3da_resp_t r;
	int rc;

	memset(join, 0, sizeof(*join));
	if (c->u.v3d.hello.proto < V3DA_PROTO_BO_SYNC) {
		return -ENOSYS;
	}
	sync_req(&q, ns, port, id);
	q.fence = *f;
	rc = vcall(c, V3DA_OP_BO_ATTACH_FENCE, &q, sizeof(q), &r);
	if (rc == 0) {
		*join = r.u.fence.fence;
	}
	return rc;
}


void drmphx_v3d_implicit_join(uint32_t port, uint64_t id, const v3da_fence_t *join)
{
	drmphx_v3d_bo_t *b;
	uint32_t i;

	/* The join fence implies every fence the BO waited for before (the server made it
	 * depend on them), so it may replace this process's mirrored last use. */
	(void)pthread_mutex_lock(&IMP.lock);
	for (i = 0u; i < DRMPHX_MAX_IMPLICIT; i++) {
		drmphx_conn_t *c = IMP.e[i].conn;
		if ((c == NULL) || (IMP.e[i].port != port) || (IMP.e[i].id != id)) {
			continue;
		}
		(void)pthread_mutex_lock(&c->lock);
		b = bo_get(c, IMP.e[i].handle);
		if (b != NULL) {
			b->last = *join;
		}
		(void)pthread_mutex_unlock(&c->lock);
	}
	(void)pthread_mutex_unlock(&IMP.lock);
}


/* G6 flip fence of a render BO scanned out through card0 (a G7 import): the newest
 * pending fence; the rest are implied by it, because the server made the newest job
 * wait for every OTHER client's pending use (implicit dependencies) - except this
 * client's own work on another queue, which is waited for here (normally done).
 * -ENOSYS: no render connection, or a server before G6. */
static int buffer_flip_fence(uint32_t port, uint64_t id, v3da_fence_t *f)
{
	v3da_fence_t set[DRMPHX_SYNCFILE_FENCES];
	drmphx_conn_t *c;
	uint32_t n = 0u, i;
	int dev, rc;

	if (drmphx_any_v3d(&c, &dev) != 0) {
		return -ENOSYS;
	}
	rc = drmphx_v3d_buffer_fences(c, DRMPHX_NS_V3DBUF, port, id, set, DRMPHX_SYNCFILE_FENCES, &n);
	for (i = 1u; (rc == 0) && (i < n); i++) {
		if ((set[i].slot == set[0].slot) && (set[i].queue != set[0].queue)) {
			rc = fence_wait(c, &set[i], FOREVER_NS);
		}
	}
	if ((rc == 0) && (n != 0u) && (fence_signaled(c, &set[0]) == 0)) {
		*f = set[0];
		rc = 1;
	}
	drmphx_put(c);
	return rc;
}


int drmphx_v3d_flip_fence(uint32_t kms_buf_port, uint32_t port, uint64_t id, v3da_fence_t *f)
{
	int rc;

	if (port != kms_buf_port) {
		rc = buffer_flip_fence(port, id, f);
		if (rc != -ENOSYS) {
			return (rc < 0) ? 0 : rc;   /* a failed query flips ungated, as before G6 */
		}
	}
	return drmphx_v3d_implicit_fence(port, id, f);   /* G13: this process's own renders */
}


int drmphx_v3d_flip_wait(uint32_t kms_buf_port, uint32_t port, uint64_t id)
{
	v3da_fence_t f;
	drmphx_conn_t *c;
	int dev, rc = 0, rounds;

	if (port != kms_buf_port) {
		for (rounds = 0; rounds < 16; rounds++) {
			rc = buffer_flip_fence(port, id, &f);
			if (rc != 1) {
				break;
			}
			if (drmphx_any_v3d(&c, &dev) != 0) {
				break;
			}
			rc = fence_wait(c, &f, FOREVER_NS);
			drmphx_put(c);
			if (rc != 0) {
				return rc;
			}
		}
		if (rc != -ENOSYS) {
			return (rc < 0) ? rc : 0;
		}
	}
	return drmphx_v3d_implicit_wait(port, id);
}


/* ========================================================================= */
/* BO table (slot = the server handle's low 13 bits)                          */
/* ========================================================================= */

static drmphx_v3d_bo_t *bo_slot(drmphx_conn_t *c, uint32_t handle, int create)
{
	uint32_t s = handle & ((1u << V3DA_SLOT_BITS) - 1u), chunk = s / DRMPHX_V3D_BO_CHUNK;

	if ((handle == 0u) || (s == 0u) || (chunk >= DRMPHX_V3D_BO_CHUNKS)) {
		return NULL;
	}
	if (c->u.v3d.bo[chunk] == NULL) {
		if (create == 0) {
			return NULL;
		}
		c->u.v3d.bo[chunk] = calloc(DRMPHX_V3D_BO_CHUNK, sizeof(drmphx_v3d_bo_t));
		if (c->u.v3d.bo[chunk] == NULL) {
			return NULL;
		}
	}
	return &c->u.v3d.bo[chunk][s % DRMPHX_V3D_BO_CHUNK];
}


static drmphx_v3d_bo_t *bo_get(drmphx_conn_t *c, uint32_t handle)
{
	drmphx_v3d_bo_t *b = bo_slot(c, handle, 0);

	return ((b != NULL) && (b->handle == handle)) ? b : NULL;
}


static void bo_store(drmphx_conn_t *c, uint32_t handle, uint32_t gpuva, uint32_t size, const v3da_memref_t *mem,
	uint32_t imported)
{
	drmphx_v3d_bo_t *b;

	(void)pthread_mutex_lock(&c->lock);
	b = bo_slot(c, handle, 1);
	if (b != NULL) {
		memset(b, 0, sizeof(*b));
		b->handle = handle;
		b->gpuva = gpuva;
		b->size = size;
		b->imported = imported;
		if (mem != NULL) {
			b->mem = *mem;
		}
	}
	(void)pthread_mutex_unlock(&c->lock);
}


int drmphx_v3d_token_memref(drmphx_conn_t *c, uint32_t handle, v3da_memref_t *m)
{
	v3da_bo_req_t q;
	v3da_resp_t r;
	drmphx_v3d_bo_t *b;
	int rc;

	(void)pthread_mutex_lock(&c->lock);
	b = bo_get(c, handle);
	if ((b != NULL) && (b->mem.kind != V3DA_MEM_NONE)) {
		*m = b->mem;
		(void)pthread_mutex_unlock(&c->lock);
		return 0;
	}
	(void)pthread_mutex_unlock(&c->lock);

	memset(&q, 0, sizeof(q));
	q.handle = handle;
	rc = vcall(c, V3DA_OP_BO_MMAP, &q, sizeof(q), &r);   /* a BO another client created */
	if (rc != 0) {
		return rc;
	}
	(void)pthread_mutex_lock(&c->lock);
	b = bo_slot(c, handle, 1);
	if ((b != NULL) && (b->handle != handle)) {
		memset(b, 0, sizeof(*b));
		b->handle = handle;
		b->gpuva = r.u.bo.gpuva;
		b->size = r.u.bo.size;
	}
	if (b != NULL) {
		b->mem = r.u.bo.mem;
	}
	(void)pthread_mutex_unlock(&c->lock);
	*m = r.u.bo.mem;
	return 0;
}


/* ========================================================================= */
/* Syncobj mirror                                                             */
/* ========================================================================= */

static drmphx_v3d_sync_t *sync_get(drmphx_conn_t *c, uint32_t handle)
{
	uint32_t i;

	for (i = 0u; (handle != 0u) && (i < DRMPHX_V3D_MAX_SYNC); i++) {
		if (c->u.v3d.sync[i].handle == handle) {
			return &c->u.v3d.sync[i];
		}
	}
	return NULL;
}


static void sync_set(drmphx_conn_t *c, uint32_t handle, int state, const v3da_fence_t *f)
{
	drmphx_v3d_sync_t *s;
	uint32_t i;

	(void)pthread_mutex_lock(&c->lock);
	s = sync_get(c, handle);
	for (i = 0u; (s == NULL) && (i < DRMPHX_V3D_MAX_SYNC); i++) {
		if (c->u.v3d.sync[i].handle == 0u) {
			s = &c->u.v3d.sync[i];
		}
	}
	if (s != NULL) {
		s->handle = handle;
		s->state = state;
		if (f != NULL) {
			s->fence = *f;
		}
		else {
			memset(&s->fence, 0, sizeof(s->fence));
		}
	}
	(void)pthread_mutex_unlock(&c->lock);
}


static void sync_drop(drmphx_conn_t *c, uint32_t handle)
{
	drmphx_v3d_sync_t *s;

	(void)pthread_mutex_lock(&c->lock);
	s = sync_get(c, handle);
	if (s != NULL) {
		memset(s, 0, sizeof(*s));
	}
	(void)pthread_mutex_unlock(&c->lock);
}


/* The syncobj's current content: the mirror, else the server. */
static int sync_state(drmphx_conn_t *c, uint32_t handle, int *state, v3da_fence_t *f)
{
	v3da_syncobj_req_t q;
	v3da_resp_t r;
	drmphx_v3d_sync_t *s;
	int rc;

	(void)pthread_mutex_lock(&c->lock);
	s = sync_get(c, handle);
	if (s != NULL) {
		*state = s->state;
		*f = s->fence;
		(void)pthread_mutex_unlock(&c->lock);
		return 0;
	}
	(void)pthread_mutex_unlock(&c->lock);
	memset(&q, 0, sizeof(q));
	q.handle = handle;
	rc = vcall(c, V3DA_OP_SYNCOBJ_QUERY, &q, sizeof(q), &r);
	if (rc != 0) {
		return rc;
	}
	*state = (int)r.u.syncobj.state;
	*f = r.u.syncobj.fence;
	sync_set(c, handle, *state, f);
	return 0;
}


static int sync_simple(drmphx_conn_t *c, uint32_t op, uint32_t handle)
{
	v3da_syncobj_req_t q;

	memset(&q, 0, sizeof(q));
	q.handle = handle;
	return vcall(c, op, &q, sizeof(q), NULL);
}


/* ========================================================================= */
/* V3D ioctls                                                                 */
/* ========================================================================= */

static int ioc_get_param(drmphx_conn_t *c, struct drm_v3d_get_param *gp)
{
	v3da_get_param_req_t q;
	v3da_resp_t r;
	int rc;

	if (gp->pad != 0u) {
		return -EINVAL;
	}
	(void)pthread_mutex_lock(&c->lock);
	if ((gp->param < DRMPHX_V3D_NPARAM) && (c->u.v3d.have_param[gp->param] != 0u)) {
		gp->value = c->u.v3d.param[gp->param];
		(void)pthread_mutex_unlock(&c->lock);
		return 0;
	}
	(void)pthread_mutex_unlock(&c->lock);
	memset(&q, 0, sizeof(q));
	q.param = gp->param;
	rc = vcall(c, V3DA_OP_GET_PARAM, &q, sizeof(q), &r);
	if (rc != 0) {
		return rc;
	}
	gp->value = r.u.get_param.value;
	(void)pthread_mutex_lock(&c->lock);
	if (gp->param < DRMPHX_V3D_NPARAM) {
		c->u.v3d.param[gp->param] = gp->value;
		c->u.v3d.have_param[gp->param] = 1u;
	}
	(void)pthread_mutex_unlock(&c->lock);
	return 0;
}


static int ioc_create_bo(drmphx_conn_t *c, struct drm_v3d_create_bo *cb)
{
	v3da_bo_create_req_t q;
	v3da_resp_t r;
	int rc;

	if ((cb->flags & ~DRM_PHOENIX_V3D_CREATE_BO_SCANOUT) != 0u) {
		return -EINVAL;   /* Linux v3d: no create flags; Phoenix: the scan-out placement hint only */
	}
	memset(&q, 0, sizeof(q));
	q.size = cb->size;
	if (((cb->flags & DRM_PHOENIX_V3D_CREATE_BO_SCANOUT) != 0u) && (c->u.v3d.hello.proto >= V3DA_PROTO_BO_LOWMEM)) {
		q.flags = V3DA_BO_LOWMEM;   /* a proto-5 server places it below 1 GiB; older ones: dropped, anywhere */
	}
	rc = vcall(c, V3DA_OP_BO_CREATE, &q, sizeof(q), &r);
	if (rc != 0) {
		return rc;
	}
	bo_store(c, r.u.bo_create.handle, r.u.bo_create.gpuva, r.u.bo_create.size, &r.u.bo_create.mem, 0u);
	cb->handle = r.u.bo_create.handle;
	cb->offset = r.u.bo_create.gpuva;
	return 0;
}


static int ioc_close_bo(drmphx_conn_t *c, uint32_t handle)
{
	v3da_bo_req_t q;
	drmphx_v3d_bo_t *b;

	(void)pthread_mutex_lock(&c->lock);
	b = bo_get(c, handle);
	if (b != NULL) {
		memset(b, 0, sizeof(*b));
	}
	(void)pthread_mutex_unlock(&c->lock);
	implicit_forget(c, handle, 0);
	/* The server keeps the BO alive while a job still uses it, then quarantines it.
	 * CPU mappings made through drmPhoenixMmap stay valid until munmap (as DRM). */
	memset(&q, 0, sizeof(q));
	q.handle = handle;
	return vcall(c, V3DA_OP_BO_CLOSE, &q, sizeof(q), NULL);
}


static int ioc_get_bo_offset(drmphx_conn_t *c, struct drm_v3d_get_bo_offset *g)
{
	v3da_bo_req_t q;
	v3da_resp_t r;
	drmphx_v3d_bo_t *b;
	int rc;

	(void)pthread_mutex_lock(&c->lock);
	b = bo_get(c, g->handle);
	if (b != NULL) {
		g->offset = b->gpuva;
		(void)pthread_mutex_unlock(&c->lock);
		return 0;
	}
	(void)pthread_mutex_unlock(&c->lock);
	memset(&q, 0, sizeof(q));
	q.handle = g->handle;
	rc = vcall(c, V3DA_OP_BO_GET_OFFSET, &q, sizeof(q), &r);
	if (rc == 0) {
		g->offset = r.u.bo.gpuva;
	}
	return rc;
}


/* DRM WAIT_BO: timeout_ns is RELATIVE (unlike SYNCOBJ_WAIT). */
static int ioc_wait_bo(drmphx_conn_t *c, const struct drm_v3d_wait_bo *w)
{
	v3da_bo_req_t q;
	v3da_fence_t f;
	drmphx_v3d_bo_t *b;
	int64_t rel = (w->timeout_ns >= (uint64_t)INT64_MAX) ? FOREVER_NS : (int64_t)w->timeout_ns;
	uint64_t deadline = drmphx_now_us() + ((rel > 0) ? (uint64_t)rel / 1000u : 0u);
	int known = 0, rc;

	memset(&f, 0, sizeof(f));
	(void)pthread_mutex_lock(&c->lock);
	b = bo_get(c, w->handle);
	if ((b != NULL) && (b->imported == 0u) && (b->exported == 0u)) {
		known = 1;   /* a private BO: only this client's submits use it */
		f = b->last;
	}
	(void)pthread_mutex_unlock(&c->lock);

	if (known != 0) {
		rc = fence_wait(c, &f, rel);
	}
	else {
		/* Not ours, or shared (imported / exported, G6): the server knows every client's last use. */
		do {
			memset(&q, 0, sizeof(q));
			q.handle = w->handle;
			q.timeout_ms = slice_ms(deadline, (rel < 0) ? 1 : 0);
			c->u.v3d.ipc_waits++;
			rc = vcall(c, V3DA_OP_BO_WAIT, &q, sizeof(q), NULL);
		} while ((rc == -ETIMEDOUT) && ((rel < 0) || (drmphx_now_us() < deadline)));
	}
	return (rc == -ETIMEDOUT) ? -ETIME : rc;
}


/* Flatten a submit's syncobjs: the legacy in/out fields, or a MULTI_SYNC
 * extension. CPU-job extensions (0x02-0x07) are SUBMIT_CPU's (a server gap). */
static int collect_sems(uint32_t flags, uint64_t extensions, const uint32_t *legacy_in, const uint32_t *legacy_in_flags,
	uint32_t nlegacy_in, uint32_t legacy_out, int is_cl, v3da_sem_t *in, uint32_t *nin, v3da_sem_t *out, uint32_t *nout)
{
	const struct drm_v3d_extension *ext;
	const struct drm_v3d_multi_sync *ms;
	const struct drm_v3d_sem *sems;
	uint32_t i;

	*nin = 0u;
	*nout = 0u;
	if (((flags & DRM_V3D_SUBMIT_EXTENSION) != 0u) && (extensions != 0u)) {
		for (ext = (const struct drm_v3d_extension *)(uintptr_t)extensions; ext != NULL;
				ext = (const struct drm_v3d_extension *)(uintptr_t)ext->next) {
			if (ext->id != DRM_V3D_EXT_ID_MULTI_SYNC) {
				return -ENOSYS;
			}
			ms = (const struct drm_v3d_multi_sync *)ext;
			if ((ms->in_sync_count > V3DA_SUBMIT_MAX_SEMS) || (ms->out_sync_count > V3DA_SUBMIT_MAX_SEMS)) {
				return -EINVAL;
			}
			sems = (const struct drm_v3d_sem *)(uintptr_t)ms->in_syncs;
			for (i = 0u; i < ms->in_sync_count; i++) {
				if (sems[i].point != 0u) {
					return -EINVAL;   /* timelines are off (v3dv_device.c) */
				}
				in[*nin].handle = sems[i].handle;
				in[*nin].flags = ((is_cl != 0) && (ms->wait_stage == V3D_RENDER)) ? V3DA_SEM_RENDER : 0u;
				(*nin)++;
			}
			sems = (const struct drm_v3d_sem *)(uintptr_t)ms->out_syncs;
			for (i = 0u; i < ms->out_sync_count; i++) {
				out[*nout].handle = sems[i].handle;
				out[*nout].flags = 0u;
				(*nout)++;
			}
			return 0;
		}
	}
	for (i = 0u; i < nlegacy_in; i++) {
		if (legacy_in[i] != 0u) {
			in[*nin].handle = legacy_in[i];
			in[*nin].flags = legacy_in_flags[i];
			(*nin)++;
		}
	}
	if (legacy_out != 0u) {
		out[0].handle = legacy_out;
		out[0].flags = 0u;
		*nout = 1u;
	}
	return 0;
}


/* The flat submit buffer [desc][bos][in][out], sent page-rounded (E5). */
static int submit(drmphx_conn_t *c, uint32_t op, const void *desc, uint32_t desc_size, const uint32_t *bos, uint32_t nbo,
	const v3da_sem_t *in, uint32_t nin, const v3da_sem_t *out, uint32_t nout, v3da_submit_resp_t *resp)
{
	v3da_req_t rq;
	v3da_resp_t r;
	size_t need, wire;
	uint8_t *buf, *p;
	uint32_t i;
	drmphx_v3d_bo_t *b;
	int rc;

	if ((nbo > V3DA_SUBMIT_MAX_BOS) || (nin > V3DA_SUBMIT_MAX_SEMS) || (nout > V3DA_SUBMIT_MAX_SEMS)) {
		return -EINVAL;
	}
	need = desc_size + (size_t)nbo * sizeof(uint32_t) + ((size_t)nin + nout) * sizeof(v3da_sem_t);
	wire = (need + (size_t)_PAGE_SIZE - 1u) & ~((size_t)_PAGE_SIZE - 1u);
	buf = malloc(need);
	if (buf == NULL) {
		return -ENOMEM;
	}
	p = buf;
	memcpy(p, desc, desc_size);
	p += desc_size;
	if (nbo != 0u) {
		memcpy(p, bos, (size_t)nbo * sizeof(uint32_t));
		p += (size_t)nbo * sizeof(uint32_t);
	}
	if (nin != 0u) {
		memcpy(p, in, (size_t)nin * sizeof(v3da_sem_t));
		p += (size_t)nin * sizeof(v3da_sem_t);
	}
	if (nout != 0u) {
		memcpy(p, out, (size_t)nout * sizeof(v3da_sem_t));
	}

	memset(&rq, 0, sizeof(rq));
	rq.magic = V3DA_MAGIC;
	rq.op = op;
	rq.u.submit.desc_size = desc_size;
	rq.u.submit.nbo = nbo;
	rq.u.submit.nin = nin;
	rq.u.submit.nout = nout;
	rc = drmphx_call(c, &rq, &r, buf, need, wire, NULL, 0u);
	free(buf);
	if (rc != 0) {
		return rc;
	}
	*resp = r.u.submit;

	/* Mirror: the BOs' last use and the out-syncobjs' fence. */
	(void)pthread_mutex_lock(&c->lock);
	for (i = 0u; i < nbo; i++) {
		b = bo_get(c, bos[i]);
		if (b != NULL) {
			b->last = resp->last;
		}
	}
	(void)pthread_mutex_unlock(&c->lock);
	for (i = 0u; i < nout; i++) {
		sync_set(c, out[i].handle, V3DA_SYNC_FENCE, &resp->last);
	}
	return 0;
}


static int ioc_submit_cl(drmphx_conn_t *c, const struct drm_v3d_submit_cl *s)
{
	v3da_cl_desc_t d;
	v3da_sem_t in[V3DA_SUBMIT_MAX_SEMS], out[V3DA_SUBMIT_MAX_SEMS];
	v3da_submit_resp_t r;
	uint32_t nin, nout, lin[2], lfl[2];
	int rc;

	if (s->perfmon_id != 0u) {
		return -ENOSYS;   /* gap: PERFMON_* */
	}
	memset(&d, 0, sizeof(d));
	d.bcl_start = s->bcl_start;
	d.bcl_end = s->bcl_end;
	d.rcl_start = s->rcl_start;
	d.rcl_end = s->rcl_end;
	d.qma = s->qma;
	d.qms = s->qms;
	d.qts = s->qts;
	d.flags = ((s->flags & DRM_V3D_SUBMIT_CL_FLUSH_CACHE) != 0u) ? V3DA_CL_FLUSH_CACHE : 0u;
	lin[0] = s->in_sync_bcl;
	lfl[0] = 0u;
	lin[1] = s->in_sync_rcl;
	lfl[1] = V3DA_SEM_RENDER;
	rc = collect_sems(s->flags, s->extensions, lin, lfl, 2u, s->out_sync, 1, in, &nin, out, &nout);
	if (rc != 0) {
		return rc;
	}
	return submit(c, V3DA_OP_SUBMIT_CL, &d, sizeof(d), (const uint32_t *)(uintptr_t)s->bo_handles,
		(s->bo_handles != 0u) ? s->bo_handle_count : 0u, in, nin, out, nout, &r);
}


static int ioc_submit_tfu(drmphx_conn_t *c, const struct drm_v3d_submit_tfu *t)
{
	v3da_tfu_desc_t d;
	v3da_sem_t in[V3DA_SUBMIT_MAX_SEMS], out[V3DA_SUBMIT_MAX_SEMS];
	v3da_submit_resp_t r;
	uint32_t nin, nout, bos[4], nbo = 0u, i, lfl = 0u;
	int rc;

	memset(&d, 0, sizeof(d));
	d.icfg = t->icfg;
	d.iia = t->iia;
	d.iis = t->iis;
	d.ica = t->ica;
	d.iua = t->iua;
	d.ioa = t->ioa;
	d.ios = t->ios;
	memcpy(d.coef, t->coef, sizeof(d.coef));
	for (i = 0u; i < 4u; i++) {
		if (t->bo_handles[i] != 0u) {
			bos[nbo++] = t->bo_handles[i];
		}
	}
	rc = collect_sems(t->flags, t->extensions, &t->in_sync, &lfl, 1u, t->out_sync, 0, in, &nin, out, &nout);
	if (rc != 0) {
		return rc;
	}
	return submit(c, V3DA_OP_SUBMIT_TFU, &d, sizeof(d), bos, nbo, in, nin, out, nout, &r);
}


static int ioc_submit_csd(drmphx_conn_t *c, const struct drm_v3d_submit_csd *s)
{
	v3da_csd_desc_t d;
	v3da_sem_t in[V3DA_SUBMIT_MAX_SEMS], out[V3DA_SUBMIT_MAX_SEMS];
	v3da_submit_resp_t r;
	uint32_t nin, nout, lfl = 0u;
	int rc;

	if (s->perfmon_id != 0u) {
		return -ENOSYS;
	}
	memset(&d, 0, sizeof(d));
	memcpy(d.cfg, s->cfg, sizeof(d.cfg));
	memcpy(d.coef, s->coef, sizeof(d.coef));
	rc = collect_sems(s->flags, s->extensions, &s->in_sync, &lfl, 1u, s->out_sync, 0, in, &nin, out, &nout);
	if (rc != 0) {
		return rc;
	}
	return submit(c, V3DA_OP_SUBMIT_CSD, &d, sizeof(d), (const uint32_t *)(uintptr_t)s->bo_handles,
		(s->bo_handles != 0u) ? s->bo_handle_count : 0u, in, nin, out, nout, &r);
}


/* ========================================================================= */
/* Syncobj ioctls                                                             */
/* ========================================================================= */

/* One server SYNCOBJ_WAIT of <= V3DA_SYNCOBJ_WAIT_MAX handles, looped in bounded
 * slices until the absolute deadline (INT64_MAX = forever). */
static int server_sync_wait(drmphx_conn_t *c, const uint32_t *h, uint32_t n, uint32_t vflags, int64_t abs_ns,
	uint32_t *first)
{
	v3da_syncobj_req_t q;
	v3da_resp_t r;
	int forever = (abs_ns == INT64_MAX) ? 1 : 0, rc;
	int64_t now;
	uint64_t deadline_us;

	now = drmphx_now_ns();
	deadline_us = drmphx_now_us() + ((abs_ns > now) ? (uint64_t)(abs_ns - now) / 1000u : 0u);
	do {
		memset(&q, 0, sizeof(q));
		q.count = n;
		q.flags = vflags;
		q.timeout_ms = slice_ms(deadline_us, forever);
		memcpy(q.handles, h, n * sizeof(uint32_t));
		c->u.v3d.ipc_waits++;
		rc = vcall(c, V3DA_OP_SYNCOBJ_WAIT, &q, sizeof(q), &r);
	} while ((rc == -ETIMEDOUT) && ((forever != 0) || (drmphx_now_us() < deadline_us)));
	if ((rc == 0) && (first != NULL)) {
		*first = r.u.syncobj.first;
	}
	return rc;
}


static int sync_wait(drmphx_conn_t *c, const uint32_t *handles, uint32_t n, int64_t abs_ns, uint32_t flags,
	uint32_t *first_signaled)
{
	v3da_fence_t f;
	int st, all = ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0u) ? 1 : 0, rc = 0, forever;
	uint32_t i, nsig = 0u, first = 0u, vflags = 0u, k, chunk;
	int64_t left;

	if ((n == 0u) || (handles == NULL)) {
		return -EINVAL;
	}
	forever = (abs_ns == INT64_MAX) ? 1 : 0;

	/* Fast path: every state known locally. */
	for (i = 0u; i < n; i++) {
		if (sync_state(c, handles[i], &st, &f) != 0) {
			return -ENOENT;
		}
		if ((st == V3DA_SYNC_SIGNALED) || ((st == V3DA_SYNC_FENCE) && (fence_signaled(c, &f) != 0))) {
			if (nsig++ == 0u) {
				first = i;
			}
		}
		else if ((st == V3DA_SYNC_EMPTY) && ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) == 0u)) {
			return -EINVAL;   /* DRM: an empty syncobj without WAIT_FOR_SUBMIT */
		}
	}
	if (((all != 0) && (nsig == n)) || ((all == 0) && (nsig != 0u))) {
		if (first_signaled != NULL) {
			*first_signaled = first;
		}
		return 0;
	}
	if ((forever == 0) && (abs_ns <= drmphx_now_ns())) {
		return -ETIME;
	}

	/* One handle (glFinish, WAIT_BO-style use): wait its fence directly. */
	if ((n == 1u) && (sync_state(c, handles[0], &st, &f) == 0) && (st == V3DA_SYNC_FENCE)) {
		/* FOREVER_NS is negative: clamp only a finite deadline that has passed (M3 part 2
		 * fix - the clamp used to turn drmSyncobjWait(INT64_MAX), i.e. glFinish, into a
		 * zero-timeout poll that answered -ETIME whenever the job was still running). */
		left = forever ? FOREVER_NS : (abs_ns - drmphx_now_ns());
		rc = fence_wait(c, &f, forever ? FOREVER_NS : ((left < 0) ? 0 : left));
		if ((rc == 0) && (first_signaled != NULL)) {
			*first_signaled = 0u;
		}
		return (rc == -ETIMEDOUT) ? -ETIME : rc;
	}

	if (all != 0) {
		vflags |= V3DA_SYNCOBJ_WAIT_ALL;
	}
	if ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) != 0u) {
		vflags |= V3DA_SYNCOBJ_WAIT_FOR_SUBMIT;
	}
	if (n <= V3DA_SYNCOBJ_WAIT_MAX) {
		rc = server_sync_wait(c, handles, n, vflags, abs_ns, first_signaled);
		return (rc == -ETIMEDOUT) ? -ETIME : rc;
	}
	if (all != 0) {
		for (k = 0u; (k < n) && (rc == 0); k += chunk) {
			chunk = ((n - k) < V3DA_SYNCOBJ_WAIT_MAX) ? (n - k) : V3DA_SYNCOBJ_WAIT_MAX;
			rc = server_sync_wait(c, handles + k, chunk, vflags, abs_ns, NULL);
		}
		if ((rc == 0) && (first_signaled != NULL)) {
			*first_signaled = 0u;
		}
		return (rc == -ETIMEDOUT) ? -ETIME : rc;
	}
	/* ANY over more than the server's limit: poll the chunks. */
	for (;;) {
		for (k = 0u; k < n; k += chunk) {
			chunk = ((n - k) < V3DA_SYNCOBJ_WAIT_MAX) ? (n - k) : V3DA_SYNCOBJ_WAIT_MAX;
			rc = server_sync_wait(c, handles + k, chunk, vflags, 0, &first);
			if (rc == 0) {
				if (first_signaled != NULL) {
					*first_signaled = k + first;
				}
				return 0;
			}
			if (rc != -ETIMEDOUT) {
				return rc;
			}
		}
		if ((forever == 0) && (drmphx_now_ns() >= abs_ns)) {
			return -ETIME;
		}
		usleep(500);
	}
}


static int ioc_syncobj_handle_to_fd(drmphx_conn_t *c, int fd, struct drm_syncobj_handle *h)
{
	v3da_fence_t f;
	int st, rc, nfd;

	if ((h->flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE) == 0u) {
		return -ENOSYS;   /* gap: opaque syncobj fds (cross-process syncobj sharing, V3DA_OP_SYNCOBJ_EXPORT_EXT) */
	}
	if (h->point != 0u) {
		return -EINVAL;
	}
	rc = sync_state(c, h->handle, &st, &f);
	if (rc != 0) {
		return rc;
	}
	if (st == V3DA_SYNC_EMPTY) {
		return -EINVAL;   /* DRM: nothing to export */
	}
	if (st == V3DA_SYNC_SIGNALED) {
		memset(&f, 0, sizeof(f));   /* seqno 0 = an already-signalled snapshot */
	}
	nfd = drmphx_syncfile_new(fd, &f);
	if (nfd < 0) {
		return nfd;
	}
	h->fd = nfd;
	return 0;
}


static int ioc_syncobj_fd_to_handle(drmphx_conn_t *c, struct drm_syncobj_handle *h)
{
	v3da_syncobj_import_req_t q;
	v3da_fence_t f;
	int rc;

	if ((h->flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE) == 0u) {
		return -ENOSYS;   /* gap: opaque syncobj fds */
	}
	rc = drmphx_syncfile_get(h->fd, &f);
	if (rc != 0) {
		return -EINVAL;   /* not a sync file this process exported (cross-process: gap) */
	}
	if (f.seqno == 0u) {
		rc = sync_simple(c, V3DA_OP_SYNCOBJ_SIGNAL, h->handle);
		if (rc == 0) {
			sync_set(c, h->handle, V3DA_SYNC_SIGNALED, NULL);
		}
		return rc;
	}
	memset(&q, 0, sizeof(q));
	q.handle = h->handle;
	q.fence = f;
	rc = vcall(c, V3DA_OP_SYNCOBJ_IMPORT, &q, sizeof(q), NULL);
	if (rc == 0) {
		sync_set(c, h->handle, V3DA_SYNC_FENCE, &f);
	}
	return rc;
}


static int ioc_syncobj_transfer(drmphx_conn_t *c, const struct drm_syncobj_transfer *t)
{
	v3da_syncobj_import_req_t q;
	v3da_fence_t f;
	int st, rc;

	if ((t->src_point != 0u) || (t->dst_point != 0u)) {
		return -EINVAL;   /* binary syncobjs only (timelines are off) */
	}
	rc = sync_state(c, t->src_handle, &st, &f);
	if (rc != 0) {
		return rc;
	}
	if (st == V3DA_SYNC_EMPTY) {
		return -EINVAL;
	}
	if (st == V3DA_SYNC_SIGNALED) {
		rc = sync_simple(c, V3DA_OP_SYNCOBJ_SIGNAL, t->dst_handle);
		if (rc == 0) {
			sync_set(c, t->dst_handle, V3DA_SYNC_SIGNALED, NULL);
		}
		return rc;
	}
	memset(&q, 0, sizeof(q));
	q.handle = t->dst_handle;
	q.fence = f;
	rc = vcall(c, V3DA_OP_SYNCOBJ_IMPORT, &q, sizeof(q), NULL);
	if (rc == 0) {
		sync_set(c, t->dst_handle, V3DA_SYNC_FENCE, &f);
	}
	return rc;
}


static int all_points_zero(uint64_t points_ptr, uint32_t n)
{
	uint32_t i;

	for (i = 0u; (points_ptr != 0u) && (i < n); i++) {
		if (((const uint64_t *)(uintptr_t)points_ptr)[i] != 0u) {
			return 0;
		}
	}
	return 1;
}


static int ioc_syncobj(drmphx_conn_t *c, int fd, unsigned nr, void *arg)
{
	v3da_syncobj_req_t q;
	v3da_resp_t r;
	uint32_t i;
	int rc;

	switch (nr) {
		case NR(DRM_IOCTL_SYNCOBJ_CREATE): {
			struct drm_syncobj_create *cr = arg;
			memset(&q, 0, sizeof(q));
			q.flags = ((cr->flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0u) ? V3DA_SYNCOBJ_CREATE_SIGNALED : 0u;
			rc = vcall(c, V3DA_OP_SYNCOBJ_CREATE, &q, sizeof(q), &r);
			if (rc == 0) {
				cr->handle = r.u.syncobj.handle;
				sync_set(c, cr->handle, (q.flags != 0u) ? V3DA_SYNC_SIGNALED : V3DA_SYNC_EMPTY, NULL);
			}
			return rc;
		}
		case NR(DRM_IOCTL_SYNCOBJ_DESTROY): {
			const struct drm_syncobj_destroy *d = arg;
			sync_drop(c, d->handle);
			return sync_simple(c, V3DA_OP_SYNCOBJ_DESTROY, d->handle);
		}
		case NR(DRM_IOCTL_SYNCOBJ_RESET):
		case NR(DRM_IOCTL_SYNCOBJ_SIGNAL): {
			const struct drm_syncobj_array *a = arg;
			const uint32_t *h = (const uint32_t *)(uintptr_t)a->handles;
			int reset = (nr == NR(DRM_IOCTL_SYNCOBJ_RESET)) ? 1 : 0;
			for (i = 0u; i < a->count_handles; i++) {
				rc = sync_simple(c, reset ? V3DA_OP_SYNCOBJ_RESET : V3DA_OP_SYNCOBJ_SIGNAL, h[i]);
				if (rc != 0) {
					return rc;
				}
				sync_set(c, h[i], reset ? V3DA_SYNC_EMPTY : V3DA_SYNC_SIGNALED, NULL);
			}
			return 0;
		}
		case NR(DRM_IOCTL_SYNCOBJ_WAIT): {
			struct drm_syncobj_wait *w = arg;
			if ((w->flags & ~(uint32_t)(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)) != 0u) {
				return -EINVAL;
			}
			return sync_wait(c, (const uint32_t *)(uintptr_t)w->handles, w->count_handles, w->timeout_nsec, w->flags,
				&w->first_signaled);
		}
		case NR(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT): {
			struct drm_syncobj_timeline_wait *w = arg;
			if (!all_points_zero(w->points, w->count_handles) ||
					((w->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) != 0u)) {
				return -EINVAL;   /* timelines are off (DRM_CAP_SYNCOBJ_TIMELINE = 0) */
			}
			return sync_wait(c, (const uint32_t *)(uintptr_t)w->handles, w->count_handles, w->timeout_nsec, w->flags,
				&w->first_signaled);
		}
		case NR(DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL): {
			const struct drm_syncobj_timeline_array *a = arg;
			const uint32_t *h = (const uint32_t *)(uintptr_t)a->handles;
			if (!all_points_zero(a->points, a->count_handles)) {
				return -EINVAL;
			}
			for (i = 0u; i < a->count_handles; i++) {
				rc = sync_simple(c, V3DA_OP_SYNCOBJ_SIGNAL, h[i]);
				if (rc != 0) {
					return rc;
				}
				sync_set(c, h[i], V3DA_SYNC_SIGNALED, NULL);
			}
			return 0;
		}
		case NR(DRM_IOCTL_SYNCOBJ_QUERY): {
			const struct drm_syncobj_timeline_array *a = arg;
			for (i = 0u; (i < a->count_handles) && (a->points != 0u); i++) {
				((uint64_t *)(uintptr_t)a->points)[i] = 0u;   /* binary syncobjs report point 0 */
			}
			return 0;
		}
		case NR(DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD):
			return ioc_syncobj_handle_to_fd(c, fd, arg);
		case NR(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE):
			return ioc_syncobj_fd_to_handle(c, arg);
		case NR(DRM_IOCTL_SYNCOBJ_TRANSFER):
			return ioc_syncobj_transfer(c, arg);
		default:
			return -ENOSYS;   /* SYNCOBJ_EVENTFD */
	}
}


/* ========================================================================= */
/* PRIME                                                                      */
/* ========================================================================= */

static int ioc_prime_import(drmphx_conn_t *c, struct drm_prime_handle *ph)
{
	v3da_bo_import_req_t q;
	v3da_req_t rq;
	v3da_resp_t r;
	kms_memref_t m;
	uint32_t kms_port;
	oid_t dev;
	int rc, have;

	rc = drmphx_prime_fd_lookup(ph->fd, &m);
	if (rc != 0) {
		return (rc == -EBADF) ? -EBADF : -EINVAL;
	}
	memset(&q, 0, sizeof(q));
	q.port = m.port;
	q.cache = m.cache;
	q.id = m.addr;
	q.size = m.size;   /* 0 = unknown here (another process exported it): the server sizes it (G3) */
	kms_port = (lookup(KMS_BUF_NS, NULL, &dev) == 0) ? dev.port : 0u;
	q.ns = (m.port == kms_port) ? DRMPHX_NS_KMSBUF : DRMPHX_NS_V3DBUF;
	if ((q.ns == DRMPHX_NS_V3DBUF) && (m.addr <= 0xffffffffu)) {
		/* A /v3dbuf id is the server's BO handle: one this connection already holds (its
		 * own export, or an earlier import) is returned as is - DRM, no extra reference. */
		(void)pthread_mutex_lock(&c->lock);
		have = (bo_get(c, (uint32_t)m.addr) != NULL) ? 1 : 0;
		(void)pthread_mutex_unlock(&c->lock);
		if (have != 0) {
			ph->handle = (uint32_t)m.addr;
			return 0;
		}
	}

	memset(&rq, 0, sizeof(rq));
	rq.magic = V3DA_MAGIC;
	rq.op = V3DA_OP_BO_IMPORT;   /* M3 part 2 servers import; a part-2 server answers -ENOSYS (the old gap G1) */
	rq.u.bo_import = q;
	rc = drmphx_call(c, &rq, &r, NULL, 0u, 0u, NULL, 0u);
	if (rc != 0) {
		return rc;
	}
	(void)pthread_mutex_lock(&c->lock);
	have = (bo_get(c, r.u.bo_create.handle) != NULL) ? 1 : 0;
	(void)pthread_mutex_unlock(&c->lock);
	if (have == 0) {   /* a handle this connection holds already keeps its record (fences, origin) */
		bo_store(c, r.u.bo_create.handle, r.u.bo_create.gpuva, r.u.bo_create.size, &r.u.bo_create.mem, 1u);
	}
	(void)pthread_mutex_lock(&c->lock);
	{
		drmphx_v3d_bo_t *b = bo_get(c, r.u.bo_create.handle);
		if ((b != NULL) && (b->imported != 0u)) {   /* where it came from, for a re-export (G4a) */
			b->imp_port = q.port;
			b->imp_cache = q.cache;
			b->imp_id = q.id;
		}
	}
	(void)pthread_mutex_unlock(&c->lock);
	implicit_note(c, q.port, q.id, r.u.bo_create.handle);   /* G13: flips of this buffer wait for its renders */
	ph->handle = r.u.bo_create.handle;
	return 0;
}


/* PRIME export. A BO this server allocated (G4): V3DA_OP_BO_EXPORT publishes it
 * under the server's /v3dbuf namespace and the dma-buf descriptor is an open() of
 * "/v3dbuf/<id>" - fstat, lseek(SEEK_END) and mmap work on it as on a /kmsbuf
 * descriptor, and it passes to other processes over SCM_RIGHTS. An IMPORTED buffer
 * (G4a, M5) is exported by opening the exporter's buffer name again -- the same
 * pages, zero-copy, which is what DRM gives for a re-export of an imported GEM
 * object. That is the v3dv WSI path: swapchain memory is a card0 dumb buffer imported
 * here (device_alloc_for_wsi), then vkGetMemoryFdKHR exports it from the render node
 * and wsi_common_display imports that descriptor back on card0, where it resolves to
 * the original dumb handle. */
static int ioc_prime_export(drmphx_conn_t *c, struct drm_prime_handle *ph)
{
	kms_memref_t m;
	v3da_bo_req_t q;
	v3da_resp_t r;
	char path[48];
	const char *ns;
	uint32_t kms_port;
	oid_t dev;
	int found = 0, imported = 0, bfd, rc;

	memset(&m, 0, sizeof(m));
	(void)pthread_mutex_lock(&c->lock);
	{
		const drmphx_v3d_bo_t *b = bo_get(c, ph->handle);
		if (b != NULL) {
			found = 1;
			imported = (b->imported != 0u) && (b->imp_port != 0u);
			m.kind = KMS_MEM_OID;
			m.cache = (uint16_t)b->imp_cache;
			m.port = b->imp_port;
			m.size = b->size;
			m.addr = b->imp_id;
		}
	}
	(void)pthread_mutex_unlock(&c->lock);
	if (found == 0) {
		return -ENOENT;
	}
	if (imported != 0) {
		kms_port = (lookup(KMS_BUF_NS, NULL, &dev) == 0) ? dev.port : 0u;
		ns = (m.port == kms_port) ? KMS_BUF_NS : V3DA_BUF_NS;
	}
	else {
		if (c->u.v3d.hello.proto < V3DA_PROTO_BO_EXPORT) {
			return -ENOSYS;   /* a proto-2 render server (before G4) has no BO_EXPORT */
		}
		memset(&q, 0, sizeof(q));
		q.handle = ph->handle;
		rc = vcall(c, V3DA_OP_BO_EXPORT, &q, sizeof(q), &r);
		if (rc != 0) {
			return rc;
		}
		if (r.u.bo.mem.kind != V3DA_MEM_OID) {
			return -EPROTO;
		}
		m.kind = KMS_MEM_OID;
		m.cache = r.u.bo.mem.cache;
		m.port = r.u.bo.mem.port;
		m.size = r.u.bo.mem.size;
		m.addr = r.u.bo.mem.addr;
		ns = V3DA_BUF_NS;
		/* G13 for G7: a flip on card0 of this export (imported there by this process)
		 * waits for this BO's renders, exactly like a flip of an imported dumb buffer. */
		implicit_note(c, m.port, m.addr, ph->handle);
		(void)pthread_mutex_lock(&c->lock);
		{
			drmphx_v3d_bo_t *b = bo_get(c, ph->handle);
			if (b != NULL) {
				b->exported = 1u;   /* G6: WAIT_BO now asks the server (other processes may use it) */
			}
		}
		(void)pthread_mutex_unlock(&c->lock);
	}
	(void)snprintf(path, sizeof(path), "%s/%llu", ns, (unsigned long long)m.addr);
	bfd = open(path, O_RDONLY | (((ph->flags & DRM_CLOEXEC) != 0u) ? O_CLOEXEC : 0));   /* O_RDONLY: E1 */
	if (bfd < 0) {
		return -errno;
	}
	drmphx_prime_fd_note(bfd, &m, DRMPHX_SRV_V3D, ph->handle);
	ph->fd = bfd;
	return 0;
}


/* ========================================================================= */
/* Dispatcher                                                                 */
/* ========================================================================= */

int drmphx_v3d_ioctl(drmphx_conn_t *c, int fd, unsigned nr, void *arg)
{
	if (c->srv != DRMPHX_SRV_V3D) {
		return -EINVAL;
	}
	switch (nr) {
		case NR(DRM_IOCTL_GET_CAP): {
			struct drm_get_cap *g = arg;
			switch (g->capability) {
				case DRM_CAP_SYNCOBJ:             g->value = 1u; return 0;
				case DRM_CAP_SYNCOBJ_TIMELINE:    g->value = 0u; return 0;   /* v3dv runs without */
				case DRM_CAP_PRIME:               g->value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT; return 0;
				case DRM_CAP_TIMESTAMP_MONOTONIC: g->value = 1u; return 0;
				case DRM_CAP_DUMB_BUFFER:         g->value = 0u; return 0;   /* dumb BOs live on card0 */
				default:                          return -EINVAL;
			}
		}
		case NR(DRM_IOCTL_SET_CLIENT_CAP):
			return -EOPNOTSUPP;   /* no KMS on this node */
		case NR(DRM_IOCTL_SET_MASTER):
		case NR(DRM_IOCTL_DROP_MASTER):
		case NR(DRM_IOCTL_AUTH_MAGIC):
			return (c->node_type == DRM_NODE_PRIMARY) ? 0 : -EACCES;

		case NR(DRM_IOCTL_GEM_CLOSE):
			return ioc_close_bo(c, ((const struct drm_gem_close *)arg)->handle);
		case NR(DRM_IOCTL_PRIME_HANDLE_TO_FD):
			return ioc_prime_export(c, arg);   /* own BOs: BO_EXPORT (G4); imported BOs: the exporter's name (G4a) */
		case NR(DRM_IOCTL_PRIME_FD_TO_HANDLE):
			return ioc_prime_import(c, arg);
		case NR(DRM_IOCTL_GEM_FLINK):
		case NR(DRM_IOCTL_GEM_OPEN):
			return -ENOSYS;

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
		case NR(DRM_IOCTL_SYNCOBJ_EVENTFD):
			return ioc_syncobj(c, fd, nr, arg);

		case V3D_NR(DRM_V3D_GET_PARAM):
			return ioc_get_param(c, arg);
		case V3D_NR(DRM_V3D_CREATE_BO):
			return ioc_create_bo(c, arg);
		case V3D_NR(DRM_V3D_MMAP_BO): {
			struct drm_v3d_mmap_bo *m = arg;
			v3da_memref_t mem;
			int rc;
			if (m->flags != 0u) {
				return -EINVAL;
			}
			rc = drmphx_v3d_token_memref(c, m->handle, &mem);
			if (rc == 0) {
				m->offset = DRMPHX_TOKEN(m->handle);   /* for drmPhoenixMmap(), not mmap() */
			}
			return rc;
		}
		case V3D_NR(DRM_V3D_GET_BO_OFFSET):
			return ioc_get_bo_offset(c, arg);
		case V3D_NR(DRM_V3D_WAIT_BO):
			return ioc_wait_bo(c, arg);
		case V3D_NR(DRM_V3D_SUBMIT_CL):
			return ioc_submit_cl(c, arg);
		case V3D_NR(DRM_V3D_SUBMIT_TFU):
			return ioc_submit_tfu(c, arg);
		case V3D_NR(DRM_V3D_SUBMIT_CSD):
			return ioc_submit_csd(c, arg);
		case V3D_NR(DRM_V3D_SUBMIT_CPU):
		case V3D_NR(DRM_V3D_PERFMON_CREATE):
		case V3D_NR(DRM_V3D_PERFMON_DESTROY):
		case V3D_NR(DRM_V3D_PERFMON_GET_VALUES):
		case V3D_NR(DRM_V3D_PERFMON_GET_COUNTER):
		case V3D_NR(DRM_V3D_PERFMON_SET_GLOBAL):
			return -ENOSYS;   /* gaps: CPU queue (v3dv queries, indirect CSD), perfmons (server reserved) */

		default:
			if ((nr >= 0xa0u) && (nr <= 0xcfu)) {
				return -EOPNOTSUPP;   /* KMS (MODE_*) on the render server: not a KMS device */
			}
			return -EINVAL;
	}
}
