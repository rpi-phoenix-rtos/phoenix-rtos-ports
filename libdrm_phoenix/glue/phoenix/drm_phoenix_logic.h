/*
 * Phoenix-RTOS
 *
 * libdrm-phoenix - pure marshalling logic (no IPC, no Phoenix-only headers)
 *
 * Everything in drm_phoenix_logic.c is a pure function of its arguments, so it
 * builds natively on the host (tools/gpu-lane/libdrm-phoenix/hosttest/) where the
 * silent marshalling errors would otherwise hide until a Pi cycle.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _DRM_PHOENIX_LOGIC_H_
#define _DRM_PHOENIX_LOGIC_H_

#include <stddef.h>
#include <stdint.h>

#include "xf86drm.h"
#include "drm.h"
#include "drm_mode.h"
#include "kms_proto.h"


/* ------------------------------------------------------------------------- */
/* ioctl request decoding                                                     */
/* ------------------------------------------------------------------------- */

/* Only the low 16 bits are trusted: the number and the 'd' group are identical in
 * the Phoenix (BSD) and the Linux _IOC layouts; the size bits are not (E7 §2.1). */
#define DRMPHX_IOC_NR(req)   ((unsigned)((req) & 0xffu))
#define DRMPHX_IOC_TYPE(req) ((unsigned)(((req) >> 8) & 0xffu))

/* MMAP_BO / MAP_DUMB answer an opaque, page-aligned token instead of a
 * kernel-style fake offset: drmPhoenixMmap() turns (fd, token) back into the
 * buffer's memref. A raw mmap(drm_fd, token) fails loudly (the servers refuse
 * atSize, E1 section 3). */
#define DRMPHX_TOKEN_TAG    (1ull << 52)
#define DRMPHX_TOKEN(h)     (DRMPHX_TOKEN_TAG | ((uint64_t)(h) << 12))
#define DRMPHX_TOKEN_OK(o)  ((((uint64_t)(o)) & (DRMPHX_TOKEN_TAG | 0xfffull)) == DRMPHX_TOKEN_TAG)
#define DRMPHX_TOKEN_HANDLE(o) ((uint32_t)((((uint64_t)(o)) & ~DRMPHX_TOKEN_TAG) >> 12))


/* ------------------------------------------------------------------------- */
/* Object classification (rpi4-kms static ids, kms_proto.h)                   */
/* ------------------------------------------------------------------------- */

enum drmphx_obj {
	DRMPHX_OBJ_NONE = 0,
	DRMPHX_OBJ_CONNECTOR,
	DRMPHX_OBJ_ENCODER,
	DRMPHX_OBJ_CRTC,
	DRMPHX_OBJ_PLANE
};

int drmphx_obj_kind(uint32_t id);


/* ------------------------------------------------------------------------- */
/* Atomic flattening                                                          */
/* ------------------------------------------------------------------------- */

/* The current (complete) state of one plane, the baseline a partial DRM atomic
 * request is overlaid on. Returns 0 or a negative errno. */
typedef int (*drmphx_plane_state_fn)(void *ctx, uint32_t plane_id, kms_atomic_plane_t *st);

typedef struct {
	uint32_t crtc_id;          /* the commit's CRTC (resolved; never 0 on success) */
	int crtc_named;            /* a CRTC object appeared in the request */
	uint32_t active;           /* CRTC ACTIVE (1 unless the request clears it) */
	uint32_t mode_blob;        /* CRTC MODE_ID, 0 = unchanged */
	uint64_t out_fence_ptr;    /* CRTC OUT_FENCE_PTR, 0 = none */
	uint32_t nplanes;
	kms_atomic_plane_t st[KMS_ATOMIC_MAX_PLANES];
	int32_t in_fence_fd[KMS_ATOMIC_MAX_PLANES];   /* -1 = none; the caller resolves it to st[].in_fence */
} drmphx_atomic_t;

/* Groups a DRM atomic request (the four parallel arrays of struct drm_mode_atomic)
 * into complete per-plane states for KMS_OP_ATOMIC. Returns 0 or a negative errno:
 * -ENOENT unknown object or property, -EINVAL immutable or out-of-range value,
 * -E2BIG more planes than KMS_ATOMIC_MAX_PLANES, or the callback's error. */
int drmphx_atomic_flatten(const uint32_t *objs, const uint32_t *count_props, const uint32_t *props,
	const uint64_t *values, uint32_t count_objs, drmphx_plane_state_fn baseline, void *ctx, drmphx_atomic_t *out);

/* Fills a plane state from an object property list (as KMS_OP_GET_PROPERTIES
 * returns it for a plane). Unlisted fields stay as they were in *st. */
void drmphx_plane_state_from_props(uint32_t plane_id, const kms_prop_value_t *pv, uint32_t n, kms_atomic_plane_t *st);


/* ------------------------------------------------------------------------- */
/* GETPROPERTY / VERSION fill                                                 */
/* ------------------------------------------------------------------------- */

/* Writes the DRM view of one property into *gp (and its user arrays, each only up
 * to the capacity the caller gave: count_values / count_enum_blobs in *gp on
 * entry). data = the server's o.data: kms_prop_enum_t[nenums] for enum/bitmask,
 * uint64_t[nvalues] otherwise. */
void drmphx_fill_property(const kms_property_t *p, const void *data, size_t data_size, struct drm_mode_get_property *gp);

/* DRM_IOCTL_VERSION semantics: copies up to each *_len bytes, sets each *_len
 * to the full length. */
void drmphx_fill_version(struct drm_version *v, int major, int minor, int patch, const char *name, const char *date,
	const char *desc);


/* ------------------------------------------------------------------------- */
/* drmDevice construction                                                     */
/* ------------------------------------------------------------------------- */

#define DRMPHX_NODE_PATH_MAX 64

/* One DRM_BUS_PLATFORM device laid out exactly as libdrm's drmDeviceAlloc does
 * (one calloc block: the struct, DRM_NODE_MAX node buffers, bus info, device
 * info) with a separately allocated NULL-terminated `compatible` array, so the
 * unmodified drmFreeDevice() frees it. nodes[i] == NULL = node absent.
 * Returns NULL on allocation failure. */
drmDevicePtr drmphx_device_new(const char *fullname, const char *const *compatible,
	const char *const nodes[DRM_NODE_MAX]);

#endif /* _DRM_PHOENIX_LOGIC_H_ */
