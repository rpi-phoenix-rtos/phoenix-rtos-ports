/*
 * Phoenix-RTOS
 *
 * libdrm-phoenix - core of the Phoenix backend
 *
 * - descriptor -> connection table: a DRM descriptor is identified by the path
 *   it was opened under (sys_fdpath, which lives on the kernel's open_file_t and
 *   so also survives dup() and SCM_RIGHTS), then HELLO on the descriptor gives
 *   the per-open client id the server stored as its oid.id. Every later request
 *   is a direct raw msgSend to {server port, client id} (the M1/M2 transport);
 * - the raw-request transport with a page-aligned bounce buffer (E5: an
 *   unaligned payload end costs a shadow page and ~30 us);
 * - drm_phoenix_ioctl(): the one entry drmIoctl() makes on Phoenix;
 * - device identity (a static list of two platform devices);
 * - drmPhoenixMmap(): MMAP_BO / MAP_DUMB tokens -> the buffer's memref;
 * - PRIME descriptor bookkeeping and the in-process sync-file emulation.
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
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/msg.h>

#include "libdrm_macros.h"
#include "xf86drm.h"
#include "xf86drm_phoenix.h"
#include "drm_phoenix_priv.h"
#include "v3d_drm.h"


/* libphoenix: the canonical path a descriptor was opened under (no header). */
extern int sys_fdpath(int fd, char *buf, size_t size);

#define DRMPHX_MAX_FD      1024
#define DRMPHX_MAX_SYNCFD  256u   /* live emulated sync files (and dups of them) per process */
#define DRMPHX_MAX_PRIMEFD 64u

typedef struct {
	drmphx_conn_t *conn;
	char path[DRMPHX_NODE_PATH_MAX];
} drmphx_fdent_t;

static struct {
	pthread_mutex_t lock;          /* descriptor table, connection list, the two fd tables */
	drmphx_fdent_t fd[DRMPHX_MAX_FD];
	drmphx_conn_t *conns;

	struct {
		int used;
		int fd;                                  /* at most one entry per descriptor number */
		uint32_t seq;                            /* registration order: a full table evicts the oldest */
		uint32_t nfence;                         /* 0 = an already-signalled snapshot */
		v3da_fence_t fence[DRMPHX_SYNCFILE_FENCES];   /* a merged sync file holds several (M5) */
	} sf[DRMPHX_MAX_SYNCFD];       /* sync-file emulation */
	uint32_t sf_seq;

	struct {
		int fd;
		int srv;
		uint32_t handle;
		kms_memref_t mem;
		char path[DRMPHX_NODE_PATH_MAX];
	} pf[DRMPHX_MAX_PRIMEFD];      /* PRIME descriptors this process created */
	uint32_t pf_next;
} G = { .lock = PTHREAD_MUTEX_INITIALIZER };


/* ========================================================================= */
/* Small helpers                                                              */
/* ========================================================================= */

int drmphx_fail(int negerr)
{
	errno = (negerr < 0) ? -negerr : EIO;
	return -1;
}


uint64_t drmphx_now_us(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}


int64_t drmphx_now_ns(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}


static size_t page_round(size_t n)
{
	return (n + (size_t)_PAGE_SIZE - 1u) & ~((size_t)_PAGE_SIZE - 1u);
}


static int port_of(const char *path, uint32_t *port)
{
	oid_t dev;

	if (lookup(path, NULL, &dev) < 0) {
		return -ENOENT;
	}
	*port = dev.port;
	return 0;
}


/* The first of two names that resolves (a canonical /dev/dri name or the name
 * the server registers today). NULL if neither does. */
static const char *resolve_node(const char *canonical, const char *legacy)
{
	uint32_t port;

	if (port_of(canonical, &port) == 0) {
		return canonical;
	}
	if (port_of(legacy, &port) == 0) {
		return legacy;
	}
	return NULL;
}


/* ========================================================================= */
/* Opt-in trace (DRMPHX_TRACE)                                                */
/* ========================================================================= */

#define TRACE_FIRST 16u    /* every call of one request number up to this count... */
#define TRACE_EVERY 256u   /* ...then one call in this many (60 Hz flip/submit loops) */

static int trace_state = -1;             /* -1 = environment not read yet */
static uint32_t trace_count[2][256];     /* [render server?][request number] */


int drmphx_trace_enabled(void)
{
	const char *s;

	if (trace_state < 0) {
		s = getenv("DRMPHX_TRACE");
		trace_state = ((s != NULL) && (s[0] != '\0') && (strcmp(s, "0") != 0)) ? 1 : 0;
	}
	return trace_state;
}


static void trace_emit(const char *line, int len)
{
	ssize_t w;

	if (len <= 0) {
		return;
	}
	w = write(STDERR_FILENO, line, ((size_t)len < 511u) ? (size_t)len : 511u);   /* one write per line */
	(void)w;
}


static const char *trace_node(const drmphx_conn_t *c)
{
	if (c == NULL) {
		return "?";
	}
	if (c->srv == DRMPHX_SRV_KMS) {
		return "card0";
	}
	return (c->node_type == DRM_NODE_PRIMARY) ? "card1" : "render";
}


#define TN(x) case DRMPHX_IOC_NR(DRM_IOCTL_##x): return "DRM_IOCTL_" #x
#define TV(x) case DRM_COMMAND_BASE + DRM_V3D_##x: return "DRM_IOCTL_V3D_" #x

static const char *trace_name(int srv, unsigned nr)
{
	switch (nr) {
		TN(VERSION); TN(GET_UNIQUE); TN(GET_MAGIC); TN(SET_VERSION); TN(GET_CAP); TN(SET_CLIENT_CAP);
		TN(SET_MASTER); TN(DROP_MASTER); TN(AUTH_MAGIC); TN(WAIT_VBLANK);
		TN(GEM_CLOSE); TN(GEM_FLINK); TN(GEM_OPEN); TN(PRIME_HANDLE_TO_FD); TN(PRIME_FD_TO_HANDLE);
		TN(MODE_GETRESOURCES); TN(MODE_GETCRTC); TN(MODE_SETCRTC); TN(MODE_CURSOR); TN(MODE_GETGAMMA);
		TN(MODE_SETGAMMA); TN(MODE_GETENCODER); TN(MODE_GETCONNECTOR); TN(MODE_GETPROPERTY);
		TN(MODE_SETPROPERTY); TN(MODE_GETPROPBLOB); TN(MODE_GETFB); TN(MODE_ADDFB); TN(MODE_RMFB);
		TN(MODE_PAGE_FLIP); TN(MODE_DIRTYFB); TN(MODE_CREATE_DUMB); TN(MODE_MAP_DUMB); TN(MODE_DESTROY_DUMB);
		TN(MODE_GETPLANERESOURCES); TN(MODE_GETPLANE); TN(MODE_SETPLANE); TN(MODE_ADDFB2);
		TN(MODE_OBJ_GETPROPERTIES); TN(MODE_OBJ_SETPROPERTY); TN(MODE_CURSOR2); TN(MODE_ATOMIC);
		TN(MODE_CREATEPROPBLOB); TN(MODE_DESTROYPROPBLOB); TN(MODE_GETFB2); TN(MODE_CLOSEFB);
		TN(CRTC_GET_SEQUENCE); TN(CRTC_QUEUE_SEQUENCE);
		TN(SYNCOBJ_CREATE); TN(SYNCOBJ_DESTROY); TN(SYNCOBJ_HANDLE_TO_FD); TN(SYNCOBJ_FD_TO_HANDLE);
		TN(SYNCOBJ_WAIT); TN(SYNCOBJ_RESET); TN(SYNCOBJ_SIGNAL); TN(SYNCOBJ_TIMELINE_WAIT);
		TN(SYNCOBJ_QUERY); TN(SYNCOBJ_TRANSFER); TN(SYNCOBJ_TIMELINE_SIGNAL); TN(SYNCOBJ_EVENTFD);
		default: break;
	}
	if (srv == DRMPHX_SRV_V3D) {
		switch (nr) {
			TV(SUBMIT_CL); TV(WAIT_BO); TV(CREATE_BO); TV(MMAP_BO); TV(GET_PARAM); TV(GET_BO_OFFSET);
			TV(SUBMIT_TFU); TV(SUBMIT_CSD); TV(PERFMON_CREATE); TV(PERFMON_DESTROY);
			TV(PERFMON_GET_VALUES); TV(SUBMIT_CPU); TV(PERFMON_GET_COUNTER); TV(PERFMON_SET_GLOBAL);
			default: break;
		}
	}
	return NULL;
}

#undef TN
#undef TV


/* The key arguments of the requests on the GBM/EGL/KMS path, read after the call
 * (inputs are unchanged, outputs filled on success). */
static void trace_args(char *b, size_t n, int srv, unsigned nr, const void *arg)
{
	char path[DRMPHX_NODE_PATH_MAX];

	b[0] = '\0';
	if (arg == NULL) {
		return;
	}
	switch (nr) {
		case DRMPHX_IOC_NR(DRM_IOCTL_GET_CAP):
		case DRMPHX_IOC_NR(DRM_IOCTL_SET_CLIENT_CAP): {
			const struct drm_get_cap *g = arg;   /* same layout as drm_set_client_cap */
			(void)snprintf(b, n, " cap=0x%llx value=0x%llx", (unsigned long long)g->capability,
				(unsigned long long)g->value);
			return;
		}
		case DRMPHX_IOC_NR(DRM_IOCTL_GEM_CLOSE):
			(void)snprintf(b, n, " handle=%u", ((const struct drm_gem_close *)arg)->handle);
			return;
		case DRMPHX_IOC_NR(DRM_IOCTL_PRIME_HANDLE_TO_FD):
		case DRMPHX_IOC_NR(DRM_IOCTL_PRIME_FD_TO_HANDLE): {
			const struct drm_prime_handle *p = arg;
			if ((p->fd < 0) || (sys_fdpath(p->fd, path, sizeof(path)) < 0)) {
				(void)snprintf(path, sizeof(path), "-");
			}
			(void)snprintf(b, n, " handle=%u flags=0x%x fd=%d fdpath=%s", p->handle, p->flags, p->fd, path);
			return;
		}
		case DRMPHX_IOC_NR(DRM_IOCTL_MODE_CREATE_DUMB): {
			const struct drm_mode_create_dumb *d = arg;
			(void)snprintf(b, n, " w=%u h=%u bpp=%u flags=0x%x handle=%u pitch=%u size=%llu", d->width, d->height,
				d->bpp, d->flags, d->handle, d->pitch, (unsigned long long)d->size);
			return;
		}
		case DRMPHX_IOC_NR(DRM_IOCTL_MODE_MAP_DUMB): {
			const struct drm_mode_map_dumb *d = arg;
			(void)snprintf(b, n, " handle=%u offset=0x%llx", d->handle, (unsigned long long)d->offset);
			return;
		}
		case DRMPHX_IOC_NR(DRM_IOCTL_MODE_DESTROY_DUMB):
			(void)snprintf(b, n, " handle=%u", ((const struct drm_mode_destroy_dumb *)arg)->handle);
			return;
		case DRMPHX_IOC_NR(DRM_IOCTL_MODE_ADDFB2): {
			const struct drm_mode_fb_cmd2 *f = arg;
			(void)snprintf(b, n, " %ux%u fmt=0x%08x flags=0x%x handle=%u pitch=%u offset=%u mod=0x%llx fb=%u",
				f->width, f->height, f->pixel_format, f->flags, f->handles[0], f->pitches[0], f->offsets[0],
				(unsigned long long)f->modifier[0], f->fb_id);
			return;
		}
		case DRMPHX_IOC_NR(DRM_IOCTL_MODE_ADDFB): {
			const struct drm_mode_fb_cmd *f = arg;
			(void)snprintf(b, n, " %ux%u bpp=%u depth=%u handle=%u pitch=%u fb=%u", f->width, f->height, f->bpp,
				f->depth, f->handle, f->pitch, f->fb_id);
			return;
		}
		case DRMPHX_IOC_NR(DRM_IOCTL_MODE_RMFB):
		case DRMPHX_IOC_NR(DRM_IOCTL_MODE_CLOSEFB):
			(void)snprintf(b, n, " fb=%u", *(const unsigned int *)arg);
			return;
		case DRMPHX_IOC_NR(DRM_IOCTL_MODE_SETCRTC):
		case DRMPHX_IOC_NR(DRM_IOCTL_MODE_GETCRTC): {
			const struct drm_mode_crtc *cr = arg;
			(void)snprintf(b, n, " crtc=0x%x fb=%u x=%u y=%u connectors=%u mode_valid=%u mode=%ux%u", cr->crtc_id,
				cr->fb_id, cr->x, cr->y, cr->count_connectors, cr->mode_valid, cr->mode.hdisplay, cr->mode.vdisplay);
			return;
		}
		case DRMPHX_IOC_NR(DRM_IOCTL_MODE_PAGE_FLIP): {
			const struct drm_mode_crtc_page_flip *pf = arg;
			(void)snprintf(b, n, " crtc=0x%x fb=%u flags=0x%x user_data=0x%llx", pf->crtc_id, pf->fb_id, pf->flags,
				(unsigned long long)pf->user_data);
			return;
		}
		case DRMPHX_IOC_NR(DRM_IOCTL_MODE_ATOMIC): {
			const struct drm_mode_atomic *a = arg;
			(void)snprintf(b, n, " flags=0x%x objs=%u", a->flags, a->count_objs);
			return;
		}
		case DRMPHX_IOC_NR(DRM_IOCTL_MODE_GETRESOURCES): {
			const struct drm_mode_card_res *r = arg;
			(void)snprintf(b, n, " fbs=%u crtcs=%u connectors=%u encoders=%u", r->count_fbs, r->count_crtcs,
				r->count_connectors, r->count_encoders);
			return;
		}
		case DRMPHX_IOC_NR(DRM_IOCTL_MODE_GETCONNECTOR): {
			const struct drm_mode_get_connector *gc = arg;
			(void)snprintf(b, n, " id=0x%x connection=%u modes=%u encoder=0x%x", gc->connector_id, gc->connection,
				gc->count_modes, gc->encoder_id);
			return;
		}
		case DRMPHX_IOC_NR(DRM_IOCTL_WAIT_VBLANK): {
			const union drm_wait_vblank *w = arg;
			(void)snprintf(b, n, " type=0x%x seq=%u", (unsigned)w->request.type, w->reply.sequence);
			return;
		}
		case DRMPHX_IOC_NR(DRM_IOCTL_SYNCOBJ_CREATE): {
			const struct drm_syncobj_create *s = arg;
			(void)snprintf(b, n, " flags=0x%x handle=%u", s->flags, s->handle);
			return;
		}
		case DRMPHX_IOC_NR(DRM_IOCTL_SYNCOBJ_DESTROY):
			(void)snprintf(b, n, " handle=%u", ((const struct drm_syncobj_destroy *)arg)->handle);
			return;
		case DRMPHX_IOC_NR(DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD):
		case DRMPHX_IOC_NR(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE): {
			const struct drm_syncobj_handle *s = arg;
			(void)snprintf(b, n, " handle=%u flags=0x%x fd=%d", s->handle, s->flags, s->fd);
			return;
		}
		case DRMPHX_IOC_NR(DRM_IOCTL_SYNCOBJ_WAIT): {
			const struct drm_syncobj_wait *s = arg;
			(void)snprintf(b, n, " count=%u flags=0x%x timeout_ns=%lld first=%u", s->count_handles, s->flags,
				(long long)s->timeout_nsec, s->first_signaled);
			return;
		}
		default:
			break;
	}
	if (srv != DRMPHX_SRV_V3D) {
		return;
	}
	switch (nr) {
		case DRM_COMMAND_BASE + DRM_V3D_CREATE_BO: {
			const struct drm_v3d_create_bo *c = arg;
			(void)snprintf(b, n, " size=%u flags=0x%x handle=%u offset=0x%x", c->size, c->flags, c->handle, c->offset);
			return;
		}
		case DRM_COMMAND_BASE + DRM_V3D_MMAP_BO: {
			const struct drm_v3d_mmap_bo *m = arg;
			(void)snprintf(b, n, " handle=%u offset=0x%llx", m->handle, (unsigned long long)m->offset);
			return;
		}
		case DRM_COMMAND_BASE + DRM_V3D_GET_BO_OFFSET: {
			const struct drm_v3d_get_bo_offset *g = arg;
			(void)snprintf(b, n, " handle=%u offset=0x%x", g->handle, g->offset);
			return;
		}
		case DRM_COMMAND_BASE + DRM_V3D_GET_PARAM: {
			const struct drm_v3d_get_param *g = arg;
			(void)snprintf(b, n, " param=%u value=0x%llx", g->param, (unsigned long long)g->value);
			return;
		}
		case DRM_COMMAND_BASE + DRM_V3D_WAIT_BO: {
			const struct drm_v3d_wait_bo *w = arg;
			(void)snprintf(b, n, " handle=%u timeout_ns=%llu", w->handle, (unsigned long long)w->timeout_ns);
			return;
		}
		case DRM_COMMAND_BASE + DRM_V3D_SUBMIT_CL: {
			const struct drm_v3d_submit_cl *s = arg;
			(void)snprintf(b, n, " bcl=0x%x-0x%x rcl=0x%x-0x%x bos=%u flags=0x%x in_bcl=%u in_rcl=%u out=%u ext=%s",
				s->bcl_start, s->bcl_end, s->rcl_start, s->rcl_end, s->bo_handle_count, s->flags, s->in_sync_bcl,
				s->in_sync_rcl, s->out_sync, (s->extensions != 0u) ? "yes" : "no");
			return;
		}
		case DRM_COMMAND_BASE + DRM_V3D_SUBMIT_TFU: {
			const struct drm_v3d_submit_tfu *s = arg;
			(void)snprintf(b, n, " bo0=%u flags=0x%x in=%u out=%u", s->bo_handles[0], s->flags, s->in_sync, s->out_sync);
			return;
		}
		case DRM_COMMAND_BASE + DRM_V3D_SUBMIT_CSD: {
			const struct drm_v3d_submit_csd *s = arg;
			(void)snprintf(b, n, " bos=%u flags=0x%x in=%u out=%u", s->bo_handle_count, s->flags, s->in_sync, s->out_sync);
			return;
		}
		default:
			break;
	}
}


static void trace_ioctl(const drmphx_conn_t *c, int srv, int fd, unsigned nr, const void *arg, int rc)
{
	char args[256], line[512], nbuf[24];
	const char *name;
	uint32_t k;
	int len;

	k = __atomic_add_fetch(&trace_count[(srv == DRMPHX_SRV_V3D) ? 1 : 0][nr & 0xffu], 1u, __ATOMIC_RELAXED);
	if ((k > TRACE_FIRST) && ((k % TRACE_EVERY) != 0u)) {
		return;
	}
	name = trace_name(srv, nr);
	if (name == NULL) {
		(void)snprintf(nbuf, sizeof(nbuf), "DRM_IOCTL_0x%02x", nr);
		name = nbuf;
	}
	trace_args(args, sizeof(args), srv, nr, arg);
	len = snprintf(line, sizeof(line), "DRMPHX ioctl node=%s fd=%d nr=0x%02x name=%s rc=%d errno=%d n=%u%s\n",
		trace_node(c), fd, nr, name, (rc == 0) ? 0 : -1, (rc == 0) ? 0 : -rc, k, args);
	trace_emit(line, len);
}


void drmphx_trace_mmap(const char *kind, int fd, off_t offset, size_t len, const void *res)
{
	char line[192];
	int n, err = errno;

	n = snprintf(line, sizeof(line), "DRMPHX mmap kind=%s fd=%d offset=0x%llx handle=%u len=%zu ptr=%p errno=%d\n", kind, fd,
		(unsigned long long)offset, DRMPHX_TOKEN_OK(offset) ? DRMPHX_TOKEN_HANDLE(offset) : 0u, len,
		(res == MAP_FAILED) ? NULL : res, (res == MAP_FAILED) ? err : 0);
	trace_emit(line, n);
	errno = err;   /* the caller of a failed mmap() reads it */
}


/* ========================================================================= */
/* Connections                                                                */
/* ========================================================================= */

static const struct {
	const char *path;
	int srv;
	int type;
} known_nodes[] = {
	{ DRMPHX_PATH_CARD0, DRMPHX_SRV_KMS, DRM_NODE_PRIMARY },
	{ DRMPHX_PATH_KMS_LEGACY, DRMPHX_SRV_KMS, DRM_NODE_PRIMARY },
	{ DRMPHX_PATH_CARD1, DRMPHX_SRV_V3D, DRM_NODE_PRIMARY },
	{ DRMPHX_PATH_RENDER, DRMPHX_SRV_V3D, DRM_NODE_RENDER },
	{ DRMPHX_PATH_V3D_LEGACY, DRMPHX_SRV_V3D, DRM_NODE_RENDER },
};


static void conn_destroy(drmphx_conn_t *c)
{
	if (c->srv == DRMPHX_SRV_V3D) {
		drmphx_v3d_release(c);
	}
	if (c->scratch != NULL) {
		(void)munmap(c->scratch, c->scratch_size);
	}
	(void)pthread_mutex_destroy(&c->lock);
	(void)pthread_mutex_destroy(&c->xfer);
	free(c);
}


static void conn_unref_locked(drmphx_conn_t *c)
{
	drmphx_conn_t **pp;

	if (--c->refs > 0) {
		return;
	}
	for (pp = &G.conns; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == c) {
			*pp = c->next;
			break;
		}
	}
	c->dead = 1;
	if (c->users == 0) {
		conn_destroy(c);
	}
}


void drmphx_put(drmphx_conn_t *c)
{
	(void)pthread_mutex_lock(&G.lock);
	c->users--;
	if ((c->dead != 0) && (c->users == 0) && (c->refs == 0)) {
		conn_destroy(c);
	}
	(void)pthread_mutex_unlock(&G.lock);
}


/* A new connection for (fd, path): resolve the server, HELLO. */
static int identify(int fd, const char *path, drmphx_conn_t **out)
{
	drmphx_conn_t *c;
	uint32_t i, port;
	int srv = DRMPHX_SRV_NONE, type = DRM_NODE_PRIMARY, rc;

	for (i = 0u; i < sizeof(known_nodes) / sizeof(known_nodes[0]); i++) {
		if (strcmp(path, known_nodes[i].path) == 0) {
			srv = known_nodes[i].srv;
			type = known_nodes[i].type;
			break;
		}
	}
	if ((srv == DRMPHX_SRV_NONE) && (strncmp(path, "/dev/", 5) != 0)) {
		return -ENOTTY;   /* not a device node: never probe other servers with DRM HELLOs */
	}
	if (port_of(path, &port) != 0) {
		return -ENODEV;
	}

	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		return -ENOMEM;
	}
	(void)pthread_mutex_init(&c->lock, NULL);
	(void)pthread_mutex_init(&c->xfer, NULL);
	c->oid.port = port;
	c->node_type = type;

	if (srv == DRMPHX_SRV_NONE) {
		/* An unknown /dev name (an alias): ask the server itself. Each server
		 * answers the other's HELLO with -ENOTTY. */
		c->srv = DRMPHX_SRV_KMS;
		rc = drmphx_kms_hello(c, fd);
		if (rc == -ENOTTY) {
			c->srv = DRMPHX_SRV_V3D;
			c->node_type = DRM_NODE_RENDER;
			rc = drmphx_v3d_hello(c, fd);
		}
	}
	else {
		c->srv = srv;
		rc = (srv == DRMPHX_SRV_KMS) ? drmphx_kms_hello(c, fd) : drmphx_v3d_hello(c, fd);
	}
	if (rc != 0) {
		conn_destroy(c);
		return (rc == -ENOTTY) ? -ENOTTY : rc;
	}
	*out = c;
	return 0;
}


/* The connection of a DRM descriptor (identified on first use, re-identified
 * when the descriptor number now names something else). Pair with drmphx_put. */
static int drmphx_get(int fd, drmphx_conn_t **out)
{
	char path[DRMPHX_NODE_PATH_MAX];
	drmphx_fdent_t *e;
	drmphx_conn_t *c = NULL, *n, *it;
	int rc;

	if ((fd < 0) || (fd >= DRMPHX_MAX_FD)) {
		return -EBADF;
	}
	rc = sys_fdpath(fd, path, sizeof(path));
	if (rc < 0) {
		return (rc == -EBADF) ? -EBADF : -ENOTTY;
	}

	(void)pthread_mutex_lock(&G.lock);
	e = &G.fd[fd];
	if ((e->conn != NULL) && (strcmp(e->path, path) == 0) &&
			((e->conn->srv != DRMPHX_SRV_V3D) || (drmphx_v3d_stale(e->conn) == 0))) {
		c = e->conn;
		c->users++;
		(void)pthread_mutex_unlock(&G.lock);
		*out = c;
		return 0;
	}
	if (e->conn != NULL) {
		conn_unref_locked(e->conn);
		e->conn = NULL;
	}
	(void)pthread_mutex_unlock(&G.lock);

	rc = identify(fd, path, &n);   /* IPC: outside the global lock */
	if (drmphx_trace_enabled() != 0) {
		char line[192];
		int len = snprintf(line, sizeof(line), "DRMPHX conn fd=%d path=%s node=%s port=%u client=%llu rc=%d\n", fd, path,
			(rc == 0) ? trace_node(n) : "?", (rc == 0) ? (unsigned)n->oid.port : 0u,
			(rc == 0) ? (unsigned long long)n->oid.id : 0ull, rc);
		trace_emit(line, len);
	}
	if (rc != 0) {
		return rc;
	}

	(void)pthread_mutex_lock(&G.lock);
	/* dup()ed descriptors (and a racing identify of this one) share one client. */
	for (it = G.conns; it != NULL; it = it->next) {
		if ((it->srv == n->srv) && (it->oid.port == n->oid.port) && (it->oid.id == n->oid.id) &&
				((it->srv != DRMPHX_SRV_V3D) || (drmphx_v3d_stale(it) == 0))) {
			c = it;
			break;
		}
	}
	if (c == NULL) {
		c = n;
		n = NULL;
		c->next = G.conns;
		G.conns = c;
	}
	if (e->conn != c) {
		if (e->conn != NULL) {
			conn_unref_locked(e->conn);
		}
		e->conn = c;
		c->refs++;
		(void)snprintf(e->path, sizeof(e->path), "%s", path);
	}
	c->users++;
	(void)pthread_mutex_unlock(&G.lock);
	if (n != NULL) {
		conn_destroy(n);
	}
	*out = c;
	return 0;
}


/* Forget a descriptor's connection (the server no longer knows our client id). */
static void drmphx_forget(int fd)
{
	(void)pthread_mutex_lock(&G.lock);
	if ((fd >= 0) && (fd < DRMPHX_MAX_FD) && (G.fd[fd].conn != NULL)) {
		conn_unref_locked(G.fd[fd].conn);
		G.fd[fd].conn = NULL;
	}
	(void)pthread_mutex_unlock(&G.lock);
}


/* ========================================================================= */
/* Transport                                                                  */
/* ========================================================================= */

int drmphx_call(drmphx_conn_t *c, const void *req, void *resp, const void *idata, size_t isize, size_t iwire,
	void *odata, size_t osize)
{
	msg_t msg;
	size_t ir = page_round(iwire), orr = page_round(osize), need = ir + orr;
	void *nb;
	int err, locked = 0;

	memset(&msg, 0, sizeof(msg));
	msg.type = mtDevCtl;
	msg.oid = c->oid;
	memcpy(msg.i.raw, req, 64);

	if ((isize != 0u) || (osize != 0u)) {
		(void)pthread_mutex_lock(&c->xfer);
		locked = 1;
		if (c->scratch_size < need) {
			nb = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
			if (nb == MAP_FAILED) {
				(void)pthread_mutex_unlock(&c->xfer);
				return -ENOMEM;
			}
			if (c->scratch != NULL) {
				(void)munmap(c->scratch, c->scratch_size);
			}
			c->scratch = nb;
			c->scratch_size = need;
		}
		if (isize != 0u) {
			memcpy(c->scratch, idata, isize);
			if (iwire > isize) {
				memset((uint8_t *)c->scratch + isize, 0, iwire - isize);
			}
			msg.i.data = c->scratch;
			msg.i.size = iwire;
		}
		if (osize != 0u) {
			msg.o.data = (uint8_t *)c->scratch + ir;
			msg.o.size = orr;
		}
	}

	err = msgSend(c->oid.port, &msg);
	if ((err == 0) && (msg.o.err >= 0) && (osize != 0u) && (odata != NULL)) {
		memcpy(odata, msg.o.data, osize);
	}
	if (locked != 0) {
		(void)pthread_mutex_unlock(&c->xfer);
	}
	if (err < 0) {
		return err;
	}
	if (msg.o.err < 0) {
		return msg.o.err;
	}
	if (resp != NULL) {
		memcpy(resp, msg.o.raw, 64);
	}
	return ((const int32_t *)(const void *)msg.o.raw)[0];   /* v3da_resp_t.err / kms_resp_t.err */
}


/* ========================================================================= */
/* drmIoctl                                                                   */
/* ========================================================================= */

#define NR(r) DRMPHX_IOC_NR(r)

/* Requests answered the same way on every node (no server state). Returns 1
 * when the request is not one of them. */
static int generic_ioctl(drmphx_conn_t *c, unsigned nr, void *arg)
{
	switch (nr) {
		case NR(DRM_IOCTL_VERSION):
			if (c->srv == DRMPHX_SRV_KMS) {
				drmphx_fill_version(arg, 0, 0, 0, KMS_DRIVER_NAME, "20260926",
					"Phoenix rpi4-kms (BCM2711 firmware planes)");
			}
			else {
				drmphx_fill_version(arg, 1, 0, 0, "v3d", "20260926", "Phoenix rpi4-v3d-async (V3D 4.2)");
			}
			return 0;

		case NR(DRM_IOCTL_GET_UNIQUE): {
			struct drm_unique *u = arg;
			const char *s = (c->srv == DRMPHX_SRV_KMS) ? "gpu" : "fec00000.v3d";
			size_t n = strlen(s);
			if ((u->unique != NULL) && (u->unique_len != 0u)) {
				memcpy(u->unique, s, (n < u->unique_len) ? n : u->unique_len);
			}
			u->unique_len = n;
			return 0;
		}

		case NR(DRM_IOCTL_GET_MAGIC):
			((struct drm_auth *)arg)->magic = (drm_magic_t)c->oid.id;
			return 0;

		case NR(DRM_IOCTL_SET_VERSION):
			return 0;   /* the interface version is fixed */

		default:
			return 1;
	}
}


int drm_phoenix_ioctl(int fd, unsigned long request, void *arg)
{
	unsigned nr = DRMPHX_IOC_NR(request);
	drmphx_conn_t *c;
	int rc, tries, srv;

	if (DRMPHX_IOC_TYPE(request) == (unsigned)DRMPHX_DMA_BUF_BASE) {
		/* G6: a dma-buf request on a buffer descriptor (Mesa's WSI sends these via drmIoctl) */
		rc = drmphx_dmabuf_ioctl(fd, nr, arg);
		return (rc == 0) ? 0 : drmphx_fail(rc);
	}
	if (DRMPHX_IOC_TYPE(request) != DRM_IOCTL_BASE) {
		if (drmphx_trace_enabled() != 0) {
			trace_ioctl(NULL, DRMPHX_SRV_NONE, fd, nr, NULL, -ENOTTY);
		}
		return drmphx_fail(-ENOTTY);
	}
	for (tries = 0; tries < 2; tries++) {
		rc = drmphx_get(fd, &c);
		if (rc != 0) {
			if (drmphx_trace_enabled() != 0) {
				trace_ioctl(NULL, DRMPHX_SRV_NONE, fd, nr, NULL, rc);
			}
			return drmphx_fail(rc);
		}
		srv = c->srv;
		rc = generic_ioctl(c, nr, arg);
		if (rc == 1) {
			rc = (c->srv == DRMPHX_SRV_KMS) ? drmphx_kms_ioctl(c, fd, nr, arg) : drmphx_v3d_ioctl(c, fd, nr, arg);
		}
		if (drmphx_trace_enabled() != 0) {
			trace_ioctl(c, srv, fd, nr, arg, rc);
		}
		drmphx_put(c);
		if (rc != -EBADF) {
			break;
		}
		/* The server does not know our client id (the descriptor was closed and
		 * its number reused for a new open): identify again, once. */
		drmphx_forget(fd);
	}
	return (rc == 0) ? 0 : drmphx_fail(rc);
}


/* ========================================================================= */
/* Device identity                                                            */
/* ========================================================================= */

/* Linux on a Pi 4 exposes the same topology: vc4 (display, card0, no render
 * node) and v3d (card1 + renderD128). v3dv's enumerate_devices() calls
 * try_device(nodes[DRM_NODE_PRIMARY]) on the v3d device unconditionally, so the
 * v3d device carries a primary node too. */
static const char *const compat_vc4[] = { "brcm,bcm2711-vc5", NULL };
static const char *const compat_v3d[] = { "brcm,2711-v3d", NULL };

static drmDevicePtr device_for(int srv)
{
	const char *nodes[DRM_NODE_MAX] = { NULL, NULL, NULL };

	if (srv == DRMPHX_SRV_KMS) {
		nodes[DRM_NODE_PRIMARY] = resolve_node(DRMPHX_PATH_CARD0, DRMPHX_PATH_KMS_LEGACY);
		if (nodes[DRM_NODE_PRIMARY] == NULL) {
			return NULL;
		}
		return drmphx_device_new("/gpu", compat_vc4, nodes);
	}
	nodes[DRM_NODE_RENDER] = resolve_node(DRMPHX_PATH_RENDER, DRMPHX_PATH_V3D_LEGACY);
	if (nodes[DRM_NODE_RENDER] == NULL) {
		return NULL;
	}
	nodes[DRM_NODE_PRIMARY] = resolve_node(DRMPHX_PATH_CARD1, DRMPHX_PATH_V3D_LEGACY);
	return drmphx_device_new("/v3dbus/v3d@7ec04000", compat_v3d, nodes);
}


int drm_phoenix_get_devices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	static const int order[2] = { DRMPHX_SRV_KMS, DRMPHX_SRV_V3D };
	drmDevicePtr d;
	int i, n = 0;

	(void)flags;
	if ((devices != NULL) && (max_devices < 0)) {
		return -EINVAL;
	}
	for (i = 0; i < 2; i++) {
		d = device_for(order[i]);
		if (d == NULL) {
			continue;
		}
		if ((devices != NULL) && (n < max_devices)) {
			devices[n++] = d;
		}
		else {
			drmFreeDevice(&d);
			if (devices == NULL) {
				n++;
			}
		}
	}
	return n;
}


int drm_phoenix_get_device2(int fd, uint32_t flags, drmDevicePtr *device)
{
	drmphx_conn_t *c;
	int rc, srv;

	(void)flags;
	if ((fd < 0) || (device == NULL)) {
		return -EINVAL;
	}
	rc = drmphx_get(fd, &c);
	if (rc != 0) {
		return rc;
	}
	srv = c->srv;
	drmphx_put(c);
	*device = device_for(srv);
	return (*device != NULL) ? 0 : -ENODEV;
}


/* dev_t on Phoenix: fstat()/stat() report st_rdev = the device server's port.
 * Since M3 part 2 (G10) rpi4-v3d-async serves /dev/dri/card1 on a port of its own,
 * so the v3d primary and render nodes have distinct dev_t; with only the legacy
 * name (an older server) both resolve to /dev/v3d-async and the render match wins. */
static int srv_of_devid(dev_t devid, int *type)
{
	uint32_t port;
	const char *p;

	p = resolve_node(DRMPHX_PATH_CARD0, DRMPHX_PATH_KMS_LEGACY);
	if ((p != NULL) && (port_of(p, &port) == 0) && ((dev_t)port == devid)) {
		*type = DRM_NODE_PRIMARY;
		return DRMPHX_SRV_KMS;
	}
	if ((port_of(DRMPHX_PATH_CARD1, &port) == 0) && ((dev_t)port == devid)) {
		uint32_t rport = 0u;
		/* a card1 name on the render port (one port for both) is the render dev_t */
		if ((port_of(DRMPHX_PATH_RENDER, &rport) != 0) || (rport != port)) {
			*type = DRM_NODE_PRIMARY;
			return DRMPHX_SRV_V3D;
		}
	}
	p = resolve_node(DRMPHX_PATH_RENDER, DRMPHX_PATH_V3D_LEGACY);
	if ((p != NULL) && (port_of(p, &port) == 0) && ((dev_t)port == devid)) {
		*type = DRM_NODE_RENDER;
		return DRMPHX_SRV_V3D;
	}
	return DRMPHX_SRV_NONE;
}


int drm_phoenix_get_device_from_devid(dev_t find_rdev, uint32_t flags, drmDevicePtr *device)
{
	int type, srv = srv_of_devid(find_rdev, &type);

	(void)flags;
	if (device == NULL) {
		return -EINVAL;
	}
	if (srv == DRMPHX_SRV_NONE) {
		return -ENODEV;
	}
	*device = device_for(srv);
	return (*device != NULL) ? 0 : -ENODEV;
}


int drm_phoenix_node_type_from_devid(dev_t devid)
{
	int type;

	return (srv_of_devid(devid, &type) == DRMPHX_SRV_NONE) ? -ENODEV : type;
}


int drm_phoenix_node_type_from_fd(int fd)
{
	drmphx_conn_t *c;
	int rc, type;

	rc = drmphx_get(fd, &c);
	if (rc != 0) {
		errno = (rc == -ENOTTY) ? EINVAL : -rc;
		return -1;
	}
	type = c->node_type;
	drmphx_put(c);
	return type;
}


char *drm_phoenix_device_name_from_fd(int fd)
{
	char path[DRMPHX_NODE_PATH_MAX];
	drmphx_conn_t *c;

	if (drmphx_get(fd, &c) != 0) {
		return NULL;
	}
	drmphx_put(c);
	if (sys_fdpath(fd, path, sizeof(path)) < 0) {
		return NULL;
	}
	return strdup(path);
}


char *drm_phoenix_minor_name_for_fd(int fd, int type)
{
	const char *p = NULL;
	drmphx_conn_t *c;
	int srv;

	if (drmphx_get(fd, &c) != 0) {
		return NULL;
	}
	srv = c->srv;
	drmphx_put(c);
	if (srv == DRMPHX_SRV_KMS) {
		p = (type == DRM_NODE_PRIMARY) ? resolve_node(DRMPHX_PATH_CARD0, DRMPHX_PATH_KMS_LEGACY) : NULL;
	}
	else if (type == DRM_NODE_RENDER) {
		p = resolve_node(DRMPHX_PATH_RENDER, DRMPHX_PATH_V3D_LEGACY);
	}
	else if (type == DRM_NODE_PRIMARY) {
		p = resolve_node(DRMPHX_PATH_CARD1, DRMPHX_PATH_V3D_LEGACY);
	}
	return (p != NULL) ? strdup(p) : NULL;
}


/* drmOpenMinor(): the legacy drmOpen*() paths (by driver name, by minor). */
int drm_phoenix_open_minor(int minor, int type)
{
	const char *p = NULL;
	int fd;

	if ((type == DRM_NODE_PRIMARY) && (minor == 0)) {
		p = resolve_node(DRMPHX_PATH_CARD0, DRMPHX_PATH_KMS_LEGACY);
	}
	else if ((type == DRM_NODE_PRIMARY) && (minor == 1)) {
		p = resolve_node(DRMPHX_PATH_CARD1, DRMPHX_PATH_V3D_LEGACY);
	}
	else if ((type == DRM_NODE_RENDER) && (minor == 128)) {
		p = resolve_node(DRMPHX_PATH_RENDER, DRMPHX_PATH_V3D_LEGACY);
	}
	if (p == NULL) {
		return -ENODEV;
	}
	fd = open(p, O_RDWR | O_CLOEXEC);
	return (fd >= 0) ? fd : -errno;
}


/* ========================================================================= */
/* Buffer mapping                                                             */
/* ========================================================================= */

static const char *ns_of_port(uint32_t port)
{
	uint32_t p;

	if ((port_of(KMS_BUF_NS, &p) == 0) && (p == port)) {
		return KMS_BUF_NS;
	}
	if ((port_of(V3DA_BUF_NS, &p) == 0) && (p == port)) {
		return V3DA_BUF_NS;
	}
	return NULL;
}


void *drmphx_map_memref(uint16_t kind, uint16_t cache, uint32_t port, uint64_t size, uint64_t addr, size_t len,
	int prot, void *hint, int fixed)
{
	char path[48];
	const char *ns;
	void *p;
	int flags = (cache == KMS_CACHE_UNCACHED) ? MAP_UNCACHED : 0, bfd;

	if (fixed != 0) {
		flags |= MAP_FIXED;
	}
	if ((len == 0u) || ((size != 0u) && (len > size))) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	if (kind == KMS_MEM_PHYS) {
		return mmap(hint, len, prot, flags | MAP_PHYSMEM | MAP_ANONYMOUS, -1, (off_t)addr);
	}
	if (kind != KMS_MEM_OID) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	ns = ns_of_port(port);
	if (ns == NULL) {
		errno = ENODEV;
		return MAP_FAILED;
	}
	(void)snprintf(path, sizeof(path), "%s/%llu", ns, (unsigned long long)addr);
	bfd = open(path, O_RDONLY);   /* O_RDONLY: O_RDWR would stat() the name (E1 section 1) */
	if (bfd < 0) {
		return MAP_FAILED;
	}
	p = mmap(hint, len, prot, flags, bfd, 0);
	(void)close(bfd);   /* the mapping holds the object (E1 window refcount) */
	return p;
}


/* mmap() for DRM descriptors: `offset` is the token MMAP_BO / MAP_DUMB answered.
 * A PRIME descriptor ("/kmsbuf/<id>") maps with the export's memory type. Any
 * other descriptor falls through to mmap() unchanged. */
static void *phx_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	drmphx_conn_t *c;
	kms_memref_t km;
	v3da_memref_t vm;
	uint32_t handle;
	int rc, fixed = ((flags & MAP_FIXED) != 0) ? 1 : 0;

	if (fd < 0) {
		return mmap(addr, length, prot, flags, fd, offset);
	}
	if (!DRMPHX_TOKEN_OK(offset)) {
		if ((offset == 0) && (drmphx_prime_fd_lookup(fd, &km) == 0)) {
			/* dma-buf mmap: the export's memory type is mandatory (E1) */
			return mmap(addr, length, prot, (flags & MAP_FIXED) | ((km.cache == KMS_CACHE_UNCACHED) ? MAP_UNCACHED : 0),
				fd, 0);
		}
		return mmap(addr, length, prot, flags, fd, offset);
	}
	rc = drmphx_get(fd, &c);
	if (rc != 0) {
		return mmap(addr, length, prot, flags, fd, offset);
	}
	handle = DRMPHX_TOKEN_HANDLE(offset);
	if (c->srv == DRMPHX_SRV_KMS) {
		rc = drmphx_kms_token_memref(c, handle, &km);
		drmphx_put(c);
		if (rc != 0) {
			errno = -rc;
			return MAP_FAILED;
		}
		return drmphx_map_memref(km.kind, km.cache, km.port, km.size, km.addr, length, prot, addr, fixed);
	}
	rc = drmphx_v3d_token_memref(c, handle, &vm);
	drmphx_put(c);
	if (rc != 0) {
		errno = -rc;
		return MAP_FAILED;
	}
	return drmphx_map_memref(vm.kind, vm.cache, vm.port, vm.size, vm.addr, length, prot, addr, fixed);
}


drm_public void *drmPhoenixMmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	void *p = phx_mmap(addr, length, prot, flags, fd, offset);

	if ((drmphx_trace_enabled() != 0) && (fd >= 0) && (DRMPHX_TOKEN_OK(offset) || (offset == 0))) {
		drmphx_trace_mmap(DRMPHX_TOKEN_OK(offset) ? "token" : "fd0", fd, offset, length, p);
	}
	return p;
}


drm_public int drmPhoenixMunmap(void *addr, size_t length)
{
	return munmap(addr, length);   /* every mapping kind is a plain mapping once made */
}


/* ========================================================================= */
/* PRIME descriptors and sync files                                           */
/* ========================================================================= */

void drmphx_prime_fd_note(int fd, const kms_memref_t *m, int srv, uint32_t handle)
{
	uint32_t k;

	(void)pthread_mutex_lock(&G.lock);
	k = G.pf_next;
	G.pf[k].fd = fd;
	G.pf[k].srv = srv;
	G.pf[k].handle = handle;
	G.pf[k].mem = *m;
	if (sys_fdpath(fd, G.pf[k].path, sizeof(G.pf[k].path)) < 0) {
		G.pf[k].path[0] = '\0';
	}
	G.pf_next = (k + 1u) % DRMPHX_MAX_PRIMEFD;
	(void)pthread_mutex_unlock(&G.lock);
}


int drmphx_prime_fd_lookup(int fd, kms_memref_t *m)
{
	char path[DRMPHX_NODE_PATH_MAX], *end;
	unsigned long long id;
	const char *ns = NULL;
	uint32_t port, i, k;

	if (sys_fdpath(fd, path, sizeof(path)) < 0) {
		return -EBADF;
	}
	if (strncmp(path, KMS_BUF_NS "/", sizeof(KMS_BUF_NS)) == 0) {
		ns = KMS_BUF_NS;
	}
	else if (strncmp(path, V3DA_BUF_NS "/", sizeof(V3DA_BUF_NS)) == 0) {
		ns = V3DA_BUF_NS;
	}
	if (ns == NULL) {
		return -EINVAL;   /* not a buffer name we know */
	}
	id = strtoull(path + strlen(ns) + 1u, &end, 10);
	if ((*end != '\0') || (port_of(ns, &port) != 0)) {
		return -EINVAL;
	}
	memset(m, 0, sizeof(*m));
	m->kind = KMS_MEM_OID;
	m->cache = KMS_CACHE_UNCACHED;   /* both namespaces export Normal-NC memory */
	m->port = port;
	m->addr = id;

	/* Size: known only for descriptors this process exported (newest first). */
	(void)pthread_mutex_lock(&G.lock);
	for (i = 0u; i < DRMPHX_MAX_PRIMEFD; i++) {
		k = (G.pf_next + DRMPHX_MAX_PRIMEFD - 1u - i) % DRMPHX_MAX_PRIMEFD;
		if ((G.pf[k].path[0] != '\0') && (strcmp(G.pf[k].path, path) == 0)) {
			m->size = G.pf[k].mem.size;
			m->cache = G.pf[k].mem.cache;
			break;
		}
	}
	(void)pthread_mutex_unlock(&G.lock);
	return 0;
}


/* Add fence f to a set, one entry per {slot, queue, gen} (completion within one
 * slot queue is ordered: the higher seqno implies the lower). Returns the new count,
 * or max + 1 when the set is full and f is a new timeline. */
static uint32_t fence_set_add(v3da_fence_t *set, uint32_t n, uint32_t max, const v3da_fence_t *f)
{
	uint32_t i;

	if (f->seqno == 0u) {
		return n;   /* signalled: contributes nothing */
	}
	for (i = 0u; i < n; i++) {
		if ((set[i].slot == f->slot) && (set[i].queue == f->queue) && (set[i].gen == f->gen)) {
			if (f->seqno > set[i].seqno) {
				set[i].seqno = f->seqno;
			}
			return n;
		}
	}
	if (n >= max) {
		return max + 1u;
	}
	set[n] = *f;
	return n + 1u;
}


/* The table entry of descriptor fd, or -1 (G.lock held). */
static int sf_find_locked(int fd)
{
	uint32_t i;

	for (i = 0u; i < DRMPHX_MAX_SYNCFD; i++) {
		if ((G.sf[i].used != 0) && (G.sf[i].fd == fd)) {
			return (int)i;
		}
	}
	return -1;
}


/* Make descriptor fd a sync file holding set[0..n) (G.lock held). An entry of the
 * same number is replaced: that descriptor was closed and the number handed out
 * again. A full table evicts the oldest entry (a later import of that descriptor
 * fails visibly: SYNCOBJ_FD_TO_HANDLE answers EINVAL). */
static void sf_put_locked(int fd, const v3da_fence_t *set, uint32_t n)
{
	uint32_t i, k, oldest = 0u;
	int e = sf_find_locked(fd);

	if (e >= 0) {
		k = (uint32_t)e;
	}
	else {
		for (i = 0u; i < DRMPHX_MAX_SYNCFD; i++) {
			if (G.sf[i].used == 0) {
				break;
			}
			if ((int32_t)(G.sf[i].seq - G.sf[oldest].seq) < 0) {
				oldest = i;
			}
		}
		k = (i < DRMPHX_MAX_SYNCFD) ? i : oldest;
	}
	G.sf[k].used = 1;
	G.sf[k].fd = fd;
	G.sf[k].seq = ++G.sf_seq;
	G.sf[k].nfence = n;
	if (n != 0u) {
		memcpy(G.sf[k].fence, set, n * sizeof(set[0]));
	}
}


static int syncfile_add(int nfd, const v3da_fence_t *set, uint32_t n)
{
	(void)pthread_mutex_lock(&G.lock);
	sf_put_locked(nfd, set, n);
	(void)pthread_mutex_unlock(&G.lock);
	return nfd;
}


int drmphx_syncfile_new(int dev_fd, const v3da_fence_t *f)
{
	int nfd = dup(dev_fd);

	if (nfd < 0) {
		return -errno;
	}
	return syncfile_add(nfd, f, (f->seqno != 0u) ? 1u : 0u);
}


/* The fence set of an emulated sync file. */
static int syncfile_set(int fd, v3da_fence_t *set, uint32_t *n)
{
	int e, rc = -EINVAL;

	if (fd < 0) {
		return -EINVAL;
	}
	(void)pthread_mutex_lock(&G.lock);
	e = sf_find_locked(fd);
	if (e >= 0) {
		*n = G.sf[e].nfence;
		memcpy(set, G.sf[e].fence, G.sf[e].nfence * sizeof(set[0]));
		rc = 0;
	}
	(void)pthread_mutex_unlock(&G.lock);
	return rc;
}


int drmphx_syncfile_dup(int oldfd, int newfd)
{
	v3da_fence_t set[DRMPHX_SYNCFILE_FENCES];
	uint32_t n = 0u;
	int e, rc = -1;

	if ((oldfd < 0) || (newfd < 0) || (oldfd == newfd)) {
		return -1;
	}
	(void)pthread_mutex_lock(&G.lock);
	e = sf_find_locked(newfd);
	if (e >= 0) {
		G.sf[e].used = 0;   /* the number was free for the kernel to hand out: that sync file is gone */
	}
	e = sf_find_locked(oldfd);
	if (e >= 0) {
		n = G.sf[e].nfence;
		memcpy(set, G.sf[e].fence, n * sizeof(set[0]));
		sf_put_locked(newfd, set, n);
		rc = (int)n;
	}
	(void)pthread_mutex_unlock(&G.lock);
	return rc;
}


void drmphx_note_dup(int oldfd, int newfd, const char *via)
{
	int n = drmphx_syncfile_dup(oldfd, newfd);

	if ((n >= 0) && (drmphx_trace_enabled() != 0)) {
		char line[96];
		int len = snprintf(line, sizeof(line), "DRMPHX sync  dup fd=%d nfd=%d via=%s fences=%d\n", oldfd, newfd, via, n);
		if (len > 0) {
			(void)write(2, line, ((size_t)len < sizeof(line)) ? (size_t)len : sizeof(line) - 1u);
		}
	}
}


/* Drop signalled fences; with keep_one, CPU-wait every pending fence but the last
 * so the set shrinks to at most one (a DRM syncobj and a kms plane in-fence hold
 * one fence). The descriptor is a dup() of the render node, so it names the render
 * connection itself. */
static int fence_set_reduce(int fd, v3da_fence_t *set, uint32_t *n, int keep_one)
{
	drmphx_conn_t *c;
	uint32_t i, m = 0u;
	int rc;

	rc = drmphx_get(fd, &c);
	if (rc != 0) {
		return rc;
	}
	if (c->srv != DRMPHX_SRV_V3D) {
		drmphx_put(c);
		return -EINVAL;
	}
	for (i = 0u; i < *n; i++) {
		if (drmphx_v3d_fence_signaled(c, &set[i]) == 0) {
			set[m++] = set[i];
		}
	}
	while ((keep_one != 0) && (m > 1u) && (rc == 0)) {
		rc = drmphx_v3d_fence_wait(c, &set[0]);   /* ordered by nothing: any order is correct */
		memmove(&set[0], &set[1], (m - 1u) * sizeof(set[0]));
		m--;
	}
	drmphx_put(c);
	*n = m;
	return rc;
}


int drmphx_syncfile_get(int fd, v3da_fence_t *f)
{
	v3da_fence_t set[DRMPHX_SYNCFILE_FENCES];
	uint32_t n = 0u;
	int rc;

	rc = syncfile_set(fd, set, &n);
	if (rc != 0) {
		return rc;
	}
	if (n > 1u) {
		rc = fence_set_reduce(fd, set, &n, 1);
		if (rc != 0) {
			return rc;
		}
	}
	if (n == 0u) {
		memset(f, 0, sizeof(*f));   /* seqno 0: signalled */
	}
	else {
		*f = set[0];
	}
	return 0;
}


int drmphx_syncfile_is(int fd)
{
	v3da_fence_t set[DRMPHX_SYNCFILE_FENCES];
	uint32_t n;

	return (syncfile_set(fd, set, &n) == 0) ? 1 : 0;
}


int drmphx_syncfile_merge(int fd1, int fd2)
{
	v3da_fence_t a[DRMPHX_SYNCFILE_FENCES], b[DRMPHX_SYNCFILE_FENCES];
	uint32_t na = 0u, nb = 0u, i, t;
	int rc, nfd;

	if ((syncfile_set(fd1, a, &na) != 0) || (syncfile_set(fd2, b, &nb) != 0)) {
		return -EINVAL;   /* not both sync files of this process (cross-process: G6) */
	}
	for (i = 0u; i < nb; i++) {
		t = fence_set_add(a, na, DRMPHX_SYNCFILE_FENCES, &b[i]);
		if (t > DRMPHX_SYNCFILE_FENCES) {
			/* full of distinct timelines: drop what already signalled, then retry;
			 * still full = wait until one set of work is done (rare: 8 timelines) */
			rc = fence_set_reduce(fd1, a, &na, 0);
			if ((rc == 0) && (na >= DRMPHX_SYNCFILE_FENCES)) {
				rc = fence_set_reduce(fd1, a, &na, 1);
			}
			if (rc != 0) {
				return rc;
			}
			t = fence_set_add(a, na, DRMPHX_SYNCFILE_FENCES, &b[i]);
		}
		na = t;
	}
	nfd = dup(fd1);   /* another dup() of the render node, like every emulated sync file */
	if (nfd < 0) {
		return -errno;
	}
	(void)fcntl(nfd, F_SETFD, FD_CLOEXEC);   /* Linux sync files are O_CLOEXEC */
	return syncfile_add(nfd, a, na);
}


int drmphx_syncfile_status(int fd, uint32_t *nfences)
{
	v3da_fence_t set[DRMPHX_SYNCFILE_FENCES];
	uint32_t n = 0u;
	int rc;

	rc = syncfile_set(fd, set, &n);
	if (rc != 0) {
		return rc;
	}
	*nfences = n;
	if (n == 0u) {
		return 1;
	}
	rc = fence_set_reduce(fd, set, &n, 0);
	if (rc != 0) {
		return rc;
	}
	return (n == 0u) ? 1 : 0;   /* 1 = every fence signalled, 0 = active */
}


/* ========================================================================= */
/* dma-buf implicit sync (gap G6)                                             */
/* ========================================================================= */

/* linux/dma-buf.h, by layout: struct dma_buf_export_sync_file and
 * struct dma_buf_import_sync_file are both { __u32 flags; __s32 fd; }. */
struct drmphx_dma_buf_sync_file {
	uint32_t flags;
	int32_t fd;
};
#define DRMPHX_DMA_BUF_SYNC_READ  (1u << 0)
#define DRMPHX_DMA_BUF_SYNC_WRITE (2u << 0)
#define DRMPHX_DMA_BUF_SYNC_RW    (DRMPHX_DMA_BUF_SYNC_READ | DRMPHX_DMA_BUF_SYNC_WRITE)


int drmphx_any_v3d(drmphx_conn_t **out, int *dev_fd)
{
	int cand[8], ncand = 0, fd, i, rc;

	/* Render nodes first: an old client may only know /dev/v3d-async, which is the
	 * same server. Validated through drmphx_get: a descriptor closed and reused
	 * since is re-identified (and skipped when it no longer is a render node). */
	(void)pthread_mutex_lock(&G.lock);
	for (i = 0; (i < 2) && (ncand < 8); i++) {
		for (fd = 0; (fd < DRMPHX_MAX_FD) && (ncand < 8); fd++) {
			const drmphx_conn_t *c = G.fd[fd].conn;
			if ((c != NULL) && (c->srv == DRMPHX_SRV_V3D) && (c->dead == 0) &&
					((c->node_type == DRM_NODE_RENDER) == (i == 0))) {
				cand[ncand++] = fd;
			}
		}
	}
	(void)pthread_mutex_unlock(&G.lock);
	for (i = 0; i < ncand; i++) {
		rc = drmphx_get(cand[i], out);
		if (rc != 0) {
			continue;
		}
		if ((*out)->srv == DRMPHX_SRV_V3D) {
			*dev_fd = cand[i];
			return 0;
		}
		drmphx_put(*out);
	}
	*out = NULL;
	return -ENODEV;
}


static void dmabuf_trace(int fd, unsigned nr, const char *path, uint32_t flags, int rc, uint32_t nfence, int sync_fd)
{
	char line[192];
	int len;

	if (drmphx_trace_enabled() == 0) {
		return;
	}
	len = snprintf(line, sizeof(line), "DRMPHX dmabuf fd=%d path=%s name=%s flags=0x%x rc=%d errno=%d fences=%u sync_fd=%d\n",
		fd, path, (nr == DRMPHX_DMA_BUF_EXPORT_NR) ? "DMA_BUF_IOCTL_EXPORT_SYNC_FILE" :
		(nr == DRMPHX_DMA_BUF_IMPORT_NR) ? "DMA_BUF_IOCTL_IMPORT_SYNC_FILE" : "DMA_BUF_IOCTL_?", flags, (rc == 0) ? 0 : -1,
		(rc == 0) ? 0 : -rc, nfence, sync_fd);
	trace_emit(line, len);
}


/* DMA_BUF_IOCTL_EXPORT_SYNC_FILE: a sync file (the in-process emulation: a dup of a
 * render descriptor + a fence set) holding the buffer's pending fences - every
 * client's, from the render server. IMPORT_SYNC_FILE: every later user of the buffer
 * also waits for the sync file's fences (BO_ATTACH_FENCE; when the server cannot
 * take a fence, the CPU waits for it here, which is equivalent). Both treat every
 * use as a write, as Linux v3d does (READ and WRITE export the same set). */
int drmphx_dmabuf_ioctl(int fd, unsigned nr, void *arg)
{
	struct drmphx_dma_buf_sync_file *s = arg;
	v3da_fence_t set[DRMPHX_SYNCFILE_FENCES], join;
	char path[DRMPHX_NODE_PATH_MAX] = "-";
	drmphx_conn_t *c = NULL;
	kms_memref_t m;
	uint32_t ns, n = 0u, i, flags = (s != NULL) ? s->flags : 0u;
	int rc, dev = -1, nfd = -1;

	if ((nr != DRMPHX_DMA_BUF_EXPORT_NR) && (nr != DRMPHX_DMA_BUF_IMPORT_NR)) {
		rc = -ENOTTY;   /* DMA_BUF_IOCTL_SYNC / SET_NAME: not emulated (as before G6) */
	}
	else if (s == NULL) {
		rc = -EFAULT;
	}
	else if (((flags & ~DRMPHX_DMA_BUF_SYNC_RW) != 0u) || ((flags & DRMPHX_DMA_BUF_SYNC_RW) == 0u)) {
		rc = -EINVAL;
	}
	else if ((rc = drmphx_prime_fd_lookup(fd, &m)) != 0) {
		rc = (rc == -EBADF) ? -EBADF : -ENOTTY;   /* not a dma-buf descriptor: as Linux */
	}
	else if (drmphx_any_v3d(&c, &dev) != 0) {
		rc = -ENOTTY;   /* no render connection here: the feature is absent (Mesa falls back) */
	}
	else if (c->u.v3d.hello.proto < V3DA_PROTO_BO_SYNC) {
		rc = -ENOTTY;   /* a render server before G6: exactly the answer before G6 */
	}
	(void)sys_fdpath(fd, path, sizeof(path));
	ns = (strncmp(path, KMS_BUF_NS "/", sizeof(KMS_BUF_NS)) == 0) ? DRMPHX_NS_KMSBUF : DRMPHX_NS_V3DBUF;

	if ((rc == 0) && (nr == DRMPHX_DMA_BUF_EXPORT_NR)) {
		rc = drmphx_v3d_buffer_fences(c, ns, m.port, m.addr, set, DRMPHX_SYNCFILE_FENCES, &n);
		if (rc == 0) {
			nfd = dup(dev);   /* like every emulated sync file */
			if (nfd < 0) {
				rc = -errno;
			}
			else {
				(void)fcntl(nfd, F_SETFD, FD_CLOEXEC);   /* Linux sync files are O_CLOEXEC */
				s->fd = syncfile_add(nfd, set, n);
			}
		}
	}
	else if ((rc == 0) && (nr == DRMPHX_DMA_BUF_IMPORT_NR)) {
		if (syncfile_set(s->fd, set, &n) != 0) {
			rc = -EINVAL;   /* not a sync file of this process (cross-process sync-file descriptors are not in G6) */
		}
		for (i = 0u; (rc == 0) && (i < n); i++) {
			if (drmphx_v3d_fence_signaled(c, &set[i]) != 0) {
				continue;
			}
			rc = drmphx_v3d_buffer_attach(c, ns, m.port, m.addr, &set[i], &join);
			if ((rc == -EBUSY) || (rc == -ENOENT)) {
				rc = drmphx_v3d_fence_wait(c, &set[i]);   /* the server cannot hold it: wait here instead */
			}
			else if ((rc == 0) && (join.seqno != 0u)) {
				drmphx_v3d_implicit_join(m.port, m.addr, &join);
			}
		}
	}
	if (c != NULL) {
		drmphx_put(c);
	}
	dmabuf_trace(fd, nr, path, flags, rc, n, (rc == 0) && (nr == DRMPHX_DMA_BUF_EXPORT_NR) ? nfd : -1);
	return rc;
}
