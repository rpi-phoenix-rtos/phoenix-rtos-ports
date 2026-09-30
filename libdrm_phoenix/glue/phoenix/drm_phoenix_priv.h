/*
 * Phoenix-RTOS
 *
 * libdrm-phoenix - internal interfaces of the Phoenix backend
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _DRM_PHOENIX_PRIV_H_
#define _DRM_PHOENIX_PRIV_H_

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/msg.h>

#include "drm_phoenix_logic.h"
#include "v3da_proto.h"
#include "kms_proto.h"
#include "drm_phoenix_ext.h"


/* The two servers this library talks to. */
enum drmphx_srv {
	DRMPHX_SRV_NONE = 0,
	DRMPHX_SRV_KMS,    /* rpi4-kms: /dev/dri/card0 (fallback /dev/kms) */
	DRMPHX_SRV_V3D     /* rpi4-v3d-async: /dev/dri/renderD128 + /dev/dri/card1 (fallback /dev/v3d-async) */
};

/* Canonical node names (what drmGetDevices2 reports when they resolve) and the
 * names the servers register today (the fallback). */
#define DRMPHX_PATH_CARD0      "/dev/dri/card0"
#define DRMPHX_PATH_CARD1      "/dev/dri/card1"
#define DRMPHX_PATH_RENDER     "/dev/dri/renderD128"
#define DRMPHX_PATH_KMS_LEGACY "/dev/" KMS_DEV_NAME
#define DRMPHX_PATH_V3D_LEGACY "/dev/" V3DA_DEV_NAME



/* ------------------------------------------------------------------------- */
/* Per-connection state (one per server client = one per open() of a node;    */
/* dup()ed descriptors share it)                                              */
/* ------------------------------------------------------------------------- */

#define DRMPHX_V3D_BO_CHUNK   256u
#define DRMPHX_V3D_BO_CHUNKS  32u     /* 8192 slots == the server's 13-bit handle slot */
#define DRMPHX_V3D_MAX_SYNC   256u
#define DRMPHX_V3D_NPARAM     32u
#define DRMPHX_KMS_MAX_DUMB   256u
#define DRMPHX_KMS_MAX_FB     64u      /* framebuffers mirrored per connection (G13: fb -> dumb handle) */
#define DRMPHX_KMS_MAX_PLANES (KMS_MAX_CRTCS * KMS_PLANES_PER_CRTC)

typedef struct {
	uint32_t handle;          /* 0 = free */
	uint32_t gpuva;
	uint32_t size;
	uint32_t imported;        /* 1 = BO_IMPORT'ed (PRIME) */
	v3da_memref_t mem;        /* kind NONE = not known yet (ask BO_MMAP) */
	v3da_fence_t last;        /* last job of this client that used it (seqno 0 = none) */
	uint32_t imp_port;        /* imported only: the exporter's buffer namespace port ... */
	uint32_t imp_cache;       /* ... the export's memory type (enum kms_mem_cache) ... */
	uint64_t imp_id;          /* ... and the object id: a render-node re-export reopens it (G4a, M5) */
	uint32_t exported;        /* 1 = this client's BO, PRIME-exported (G4): other processes may use it (G6) */
} drmphx_v3d_bo_t;

typedef struct {
	uint32_t handle;          /* 0 = free */
	int state;                /* enum v3da_syncobj_state (mirror of the server's) */
	v3da_fence_t fence;
} drmphx_v3d_sync_t;

typedef struct {
	uint32_t handle;          /* 0 = free */
	uint32_t pitch;
	uint64_t size;
	kms_memref_t mem;
} drmphx_kms_dumb_t;

typedef struct {
	uint32_t fb_id;           /* 0 = free */
	uint32_t handle;          /* the dumb handle ADDFB2 named */
	kms_memref_t mem;         /* its buffer, resolved at ADDFB2: the framebuffer outlives the handle */
} drmphx_kms_fb_t;

typedef struct drmphx_conn {
	struct drmphx_conn *next;
	int srv;                  /* enum drmphx_srv */
	int node_type;            /* DRM_NODE_PRIMARY / DRM_NODE_RENDER */
	oid_t oid;                /* {server port, client id}: direct requests */
	int refs;                 /* descriptor-table entries (global lock) */
	int users;                /* calls in progress (global lock) */
	int dead;                 /* no longer reachable from the descriptor table */

	pthread_mutex_t lock;     /* the tables below */
	pthread_mutex_t xfer;     /* the scratch buffer */
	void *scratch;            /* page-aligned bounce for i.data / o.data (E5) */
	size_t scratch_size;

	union {
		struct {
			v3da_hello_t hello;
			const volatile v3da_fence_page_t *fp;   /* read-only, cached */
			uint64_t param[DRMPHX_V3D_NPARAM];
			uint8_t have_param[DRMPHX_V3D_NPARAM];
			drmphx_v3d_bo_t *bo[DRMPHX_V3D_BO_CHUNKS];
			drmphx_v3d_sync_t sync[DRMPHX_V3D_MAX_SYNC];
			uint32_t ipc_waits;
		} v3d;
		struct {
			kms_hello_t hello;
			uint32_t buf_port;
			drmphx_kms_dumb_t dumb[DRMPHX_KMS_MAX_DUMB];
			uint8_t plane_valid[DRMPHX_KMS_MAX_PLANES];
			kms_atomic_plane_t plane[DRMPHX_KMS_MAX_PLANES];   /* last committed state (mirror) */
			drmphx_kms_fb_t fb[DRMPHX_KMS_MAX_FB];
			int no_gate;          /* G13: the server refused an in-fence (-ENODEV: started without -G) */
			uint32_t implicit;    /* G13: flips that carried an implicit (library-attached) fence */
		} kms;
	} u;
} drmphx_conn_t;


/* ------------------------------------------------------------------------- */
/* Core (xf86drm_phoenix.c)                                                   */
/* ------------------------------------------------------------------------- */

/* libdrm convention: return -1 with errno. */
int drmphx_fail(int negerr);

uint64_t drmphx_now_us(void);
int64_t drmphx_now_ns(void);

/* One raw request (req/resp are the 64-byte v3da_req_t/kms_req_t envelopes, the
 * magic already set). idata/odata are bounced through the connection's
 * page-aligned scratch buffer; i.size = iwire (>= isize: pass a page multiple to
 * keep the end aligned when the server parses by counts), o.size = osize rounded
 * up to a page. Returns the IPC error, the server's msg.o.err or the per-op err. */
int drmphx_call(drmphx_conn_t *c, const void *req, void *resp, const void *idata, size_t isize, size_t iwire,
	void *odata, size_t osize);

/* Resolve a descriptor that is not a DRM node but a buffer name (PRIME fd):
 * "/kmsbuf/<id>" today. Fills *m (kind OID, port, id); size stays 0 unless this
 * process exported it. Returns 0 or a negative errno. */
int drmphx_prime_fd_lookup(int fd, kms_memref_t *m);

/* Record/forget a PRIME fd this process created (so its size and origin are
 * known to a later import in this process). */
void drmphx_prime_fd_note(int fd, const kms_memref_t *m, int srv, uint32_t handle);

/* Sync-file emulation: an exported "sync file" is a dup() of the render node
 * descriptor plus a fence snapshot (process-local; cross-process sync files are
 * a server gap). */
#define DRMPHX_SYNCFILE_FENCES 8u   /* distinct {slot, queue} timelines one merged sync file keeps */
int drmphx_syncfile_new(int dev_fd, const v3da_fence_t *f);
/* One fence for a consumer that holds one (syncobj import, kms IN_FENCE_FD): a
 * merged sync file with several pending fences is reduced by CPU-waiting all but
 * one of them. seqno 0 = signalled. */
int drmphx_syncfile_get(int fd, v3da_fence_t *f);
/* M5 (G15, in-process): the sync_file ioctls, for __wrap_ioctl.
 * is: 1 if fd is an emulated sync file of this process.
 * merge: a new sync file (O_CLOEXEC) signalled when both are -> fd, or -errno.
 * status: 1 all signalled, 0 active, -errno; *nfences = fences it holds. */
int drmphx_syncfile_is(int fd);
int drmphx_syncfile_merge(int fd1, int fd2);
int drmphx_syncfile_status(int fd, uint32_t *nfences);

/* Map a memref (PHYS: MAP_PHYSMEM; OID: open(<ns>/<id>) + mmap). */
void *drmphx_map_memref(uint16_t kind, uint16_t cache, uint32_t port, uint64_t size, uint64_t addr, size_t len,
	int prot, void *hint, int fixed);

/* Opt-in trace (environment DRMPHX_TRACE set and not "0"): one "DRMPHX ..." line
 * on stderr per DRM ioctl (rate-limited per request number), per identified
 * descriptor and per DRM buffer mapping. Off: one load and a branch per call. */
int drmphx_trace_enabled(void);
void drmphx_trace_mmap(const char *kind, int fd, off_t offset, size_t len, const void *res);


/* ------------------------------------------------------------------------- */
/* Per-server marshalling                                                     */
/* ------------------------------------------------------------------------- */

int drmphx_kms_ioctl(drmphx_conn_t *c, int fd, unsigned nr, void *arg);
int drmphx_kms_hello(drmphx_conn_t *c, int fd);
int drmphx_kms_token_memref(drmphx_conn_t *c, uint32_t handle, kms_memref_t *m);

int drmphx_v3d_ioctl(drmphx_conn_t *c, int fd, unsigned nr, void *arg);
int drmphx_v3d_hello(drmphx_conn_t *c, int fd);
void drmphx_v3d_release(drmphx_conn_t *c);
int drmphx_v3d_stale(const drmphx_conn_t *c);
int drmphx_v3d_token_memref(drmphx_conn_t *c, uint32_t handle, v3da_memref_t *m);
/* The fence page check and a bounded-slice wait until signalled (for sync files). */
int drmphx_v3d_fence_signaled(const drmphx_conn_t *c, const v3da_fence_t *f);
int drmphx_v3d_fence_wait(drmphx_conn_t *c, const v3da_fence_t *f);

/* G13 (M3 part 2): implicit sync for flips of GPU-rendered buffers, in-process.
 * A render-node PRIME import of an exported buffer {ns port, id} is recorded; the
 * display side asks for that BO's last-use fence (mirrored from this process's own
 * submits) before a flip that carries no IN_FENCE_FD. Returns 1 and *f when the
 * fence has not signalled yet, 0 when there is nothing to wait for. */
int drmphx_v3d_implicit_fence(uint32_t port, uint64_t id, v3da_fence_t *f);
/* The CPU-wait fallback (rpi4-kms without -G): block until that fence passed. */
int drmphx_v3d_implicit_wait(uint32_t port, uint64_t id);

/* G6: cross-process implicit sync (docs/gpu-new-lane/G6-cross-process-sync.md).
 * A buffer is named {ns, port, id} (DRMPHX_NS_*); the render server keeps every
 * client's last use of it (V3DA_OP_BO_LAST_FENCE / BO_ATTACH_FENCE, proto 4).
 * any_v3d: a live render-server connection of this process and one descriptor of
 * it (pair with drmphx_put); -ENODEV when there is none. */
int drmphx_any_v3d(drmphx_conn_t **out, int *dev_fd);
void drmphx_put(drmphx_conn_t *c);
/* The buffer's pending fences, newest first (-ENOSYS: a server before G6). */
int drmphx_v3d_buffer_fences(drmphx_conn_t *c, uint32_t ns, uint32_t port, uint64_t id, v3da_fence_t *set,
	uint32_t max, uint32_t *n);
/* Make later users of the buffer wait for f too; *join = the fence that now stands
 * for the buffer's implicit fences (seqno 0: f had already signalled). */
int drmphx_v3d_buffer_attach(drmphx_conn_t *c, uint32_t ns, uint32_t port, uint64_t id, const v3da_fence_t *f,
	v3da_fence_t *join);
/* Fold a join fence into this process's mirrors of {port, id} (G13 flips of it). */
void drmphx_v3d_implicit_join(uint32_t port, uint64_t id, const v3da_fence_t *join);
/* The fence a flip of the buffer {port, id} must wait for: a buffer that is not
 * one of the display server's own dumb buffers (port != kms_buf_port: a G7 import
 * of a render BO, whose producer may be another process) is asked of the render
 * server (G6); otherwise, or against a server before G6, the G13 in-process mirror.
 * 1 = *f is pending, 0 = nothing to wait for. flip_wait: the CPU-wait fallback. */
int drmphx_v3d_flip_fence(uint32_t kms_buf_port, uint32_t port, uint64_t id, v3da_fence_t *f);
int drmphx_v3d_flip_wait(uint32_t kms_buf_port, uint32_t port, uint64_t id);
/* DMA_BUF_IOCTL_EXPORT_SYNC_FILE / IMPORT_SYNC_FILE on a buffer descriptor (drmIoctl
 * and __wrap_ioctl route ioctl type 'b' here). 0 or -errno; -ENOTTY for the other
 * dma-buf requests, a descriptor that is not a buffer, or no G6 render server. */
#define DRMPHX_DMA_BUF_BASE     'b'
#define DRMPHX_DMA_BUF_EXPORT_NR 2u   /* DMA_BUF_IOCTL_EXPORT_SYNC_FILE (Linux 6.0) */
#define DRMPHX_DMA_BUF_IMPORT_NR 3u   /* DMA_BUF_IOCTL_IMPORT_SYNC_FILE */
int drmphx_dmabuf_ioctl(int fd, unsigned nr, void *arg);

#endif /* _DRM_PHOENIX_PRIV_H_ */
