/*
 * Phoenix-RTOS
 *
 * libdrm-phoenix - pure marshalling logic (no IPC, no Phoenix-only headers)
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "drm_phoenix_logic.h"


int drmphx_obj_kind(uint32_t id)
{
	if ((id >= KMS_ID_CONNECTOR(0)) && (id < KMS_ID_CONNECTOR(KMS_MAX_CRTCS))) {
		return DRMPHX_OBJ_CONNECTOR;
	}
	if ((id >= KMS_ID_ENCODER(0)) && (id < KMS_ID_ENCODER(KMS_MAX_CRTCS))) {
		return DRMPHX_OBJ_ENCODER;
	}
	if ((id >= KMS_ID_CRTC(0)) && (id < KMS_ID_CRTC(KMS_MAX_CRTCS))) {
		return DRMPHX_OBJ_CRTC;
	}
	if ((id >= KMS_ID_PLANE(0u, 0u)) && (id < KMS_ID_PLANE(KMS_MAX_CRTCS, 0u))) {
		return DRMPHX_OBJ_PLANE;
	}
	return DRMPHX_OBJ_NONE;
}


void drmphx_plane_state_from_props(uint32_t plane_id, const kms_prop_value_t *pv, uint32_t n, kms_atomic_plane_t *st)
{
	uint32_t i;

	st->plane_id = plane_id;
	for (i = 0u; i < n; i++) {
		uint64_t v = pv[i].value;
		switch (pv[i].prop_id) {
			case KMS_PROP_FB_ID:    st->fb_id = (uint32_t)v; break;
			case KMS_PROP_CRTC_ID:  st->crtc_id = (uint32_t)v; break;
			case KMS_PROP_SRC_X:    st->src_x = (uint32_t)v; break;
			case KMS_PROP_SRC_Y:    st->src_y = (uint32_t)v; break;
			case KMS_PROP_SRC_W:    st->src_w = (uint32_t)v; break;
			case KMS_PROP_SRC_H:    st->src_h = (uint32_t)v; break;
			case KMS_PROP_CRTC_X:   st->crtc_x = (int32_t)(int64_t)v; break;
			case KMS_PROP_CRTC_Y:   st->crtc_y = (int32_t)(int64_t)v; break;
			case KMS_PROP_CRTC_W:   st->crtc_w = (uint32_t)v; break;
			case KMS_PROP_CRTC_H:   st->crtc_h = (uint32_t)v; break;
			case KMS_PROP_ZPOS:     st->zpos = (int32_t)v; break;
			case KMS_PROP_ALPHA:    st->alpha = (uint32_t)v; break;
			case KMS_PROP_ROTATION: st->rotation = (uint32_t)v; break;
			default: break;   /* type, IN_FORMATS, IN_FENCE_FD: not state */
		}
	}
}


static int plane_slot(drmphx_atomic_t *a, uint32_t plane_id, drmphx_plane_state_fn baseline, void *ctx,
	kms_atomic_plane_t **st, uint32_t *idx)
{
	uint32_t i;
	int rc;

	for (i = 0u; i < a->nplanes; i++) {
		if (a->st[i].plane_id == plane_id) {
			*st = &a->st[i];
			*idx = i;
			return 0;
		}
	}
	if (a->nplanes >= KMS_ATOMIC_MAX_PLANES) {
		return -E2BIG;
	}
	i = a->nplanes;
	memset(&a->st[i], 0, sizeof(a->st[i]));
	a->st[i].plane_id = plane_id;
	a->st[i].alpha = 0xffffu;
	a->st[i].rotation = DRM_MODE_ROTATE_0;
	if (baseline != NULL) {
		rc = baseline(ctx, plane_id, &a->st[i]);
		if (rc != 0) {
			return rc;
		}
		a->st[i].plane_id = plane_id;
	}
	memset(&a->st[i].in_fence, 0, sizeof(a->st[i].in_fence));   /* fences never carry over */
	a->in_fence_fd[i] = -1;
	a->nplanes++;
	*st = &a->st[i];
	*idx = i;
	return 0;
}


static int set_plane_prop(drmphx_atomic_t *a, uint32_t idx, kms_atomic_plane_t *st, uint32_t prop, uint64_t v)
{
	switch (prop) {
		case KMS_PROP_FB_ID:
			st->fb_id = (uint32_t)v;
			return 0;
		case KMS_PROP_CRTC_ID:
			if ((v != 0u) && (drmphx_obj_kind((uint32_t)v) != DRMPHX_OBJ_CRTC)) {
				return -EINVAL;
			}
			st->crtc_id = (uint32_t)v;
			return 0;
		case KMS_PROP_SRC_X: st->src_x = (uint32_t)v; return 0;
		case KMS_PROP_SRC_Y: st->src_y = (uint32_t)v; return 0;
		case KMS_PROP_SRC_W: st->src_w = (uint32_t)v; return 0;
		case KMS_PROP_SRC_H: st->src_h = (uint32_t)v; return 0;
		case KMS_PROP_CRTC_X: st->crtc_x = (int32_t)(int64_t)v; return 0;
		case KMS_PROP_CRTC_Y: st->crtc_y = (int32_t)(int64_t)v; return 0;
		case KMS_PROP_CRTC_W:
			if (v > 0x7fffffffu) {
				return -EINVAL;
			}
			st->crtc_w = (uint32_t)v;
			return 0;
		case KMS_PROP_CRTC_H:
			if (v > 0x7fffffffu) {
				return -EINVAL;
			}
			st->crtc_h = (uint32_t)v;
			return 0;
		case KMS_PROP_IN_FENCE_FD:
			if (((int64_t)v < -1) || ((int64_t)v > 0x7fffffff)) {
				return -EINVAL;
			}
			a->in_fence_fd[idx] = (int32_t)(int64_t)v;
			return 0;
		case KMS_PROP_ZPOS:
			if (v > 7u) {
				return -EINVAL;
			}
			st->zpos = (int32_t)v;
			return 0;
		case KMS_PROP_ALPHA:
			if (v > 0xffffu) {
				return -EINVAL;
			}
			st->alpha = (uint32_t)v;
			return 0;
		case KMS_PROP_ROTATION:
			st->rotation = (uint32_t)v;
			return 0;
		case KMS_PROP_TYPE:
		case KMS_PROP_IN_FORMATS:
			return -EINVAL;   /* immutable (DRM: -EINVAL) */
		default:
			return -ENOENT;
	}
}


static int set_crtc_prop(drmphx_atomic_t *a, uint32_t prop, uint64_t v)
{
	switch (prop) {
		case KMS_PROP_ACTIVE:
			if (v > 1u) {
				return -EINVAL;
			}
			a->active = (uint32_t)v;
			return 0;
		case KMS_PROP_MODE_ID:
			a->mode_blob = (uint32_t)v;
			return 0;
		case KMS_PROP_OUT_FENCE_PTR:
			a->out_fence_ptr = v;
			return 0;
		case KMS_PROP_VRR_ENABLED:
			return (v == 0u) ? 0 : -EINVAL;   /* vrr_capable = 0 */
		default:
			return -ENOENT;
	}
}


static int set_connector_prop(drmphx_atomic_t *a, uint32_t prop, uint64_t v)
{
	switch (prop) {
		case KMS_PROP_CRTC_ID:
			if ((v != 0u) && (drmphx_obj_kind((uint32_t)v) != DRMPHX_OBJ_CRTC)) {
				return -EINVAL;
			}
			if ((v != 0u) && (a->crtc_id == 0u)) {
				a->crtc_id = (uint32_t)v;
			}
			return 0;
		case KMS_PROP_DPMS:
			return (v <= 3u) ? 0 : -EINVAL;   /* accepted, not acted on (Stage A) */
		case KMS_PROP_LINK_STATUS:
			return (v <= 1u) ? 0 : -EINVAL;
		case KMS_PROP_EDID:
		case KMS_PROP_NON_DESKTOP:
			return -EINVAL;   /* immutable */
		default:
			return -ENOENT;
	}
}


int drmphx_atomic_flatten(const uint32_t *objs, const uint32_t *count_props, const uint32_t *props,
	const uint64_t *values, uint32_t count_objs, drmphx_plane_state_fn baseline, void *ctx, drmphx_atomic_t *out)
{
	uint32_t o, p, k = 0u, idx, i;
	kms_atomic_plane_t *st;
	int rc, kind;

	memset(out, 0, sizeof(*out));
	out->active = 1u;
	for (i = 0u; i < KMS_ATOMIC_MAX_PLANES; i++) {
		out->in_fence_fd[i] = -1;
	}

	for (o = 0u; o < count_objs; o++) {
		kind = drmphx_obj_kind(objs[o]);
		if (kind == DRMPHX_OBJ_CRTC) {
			if ((out->crtc_named != 0) && (out->crtc_id != objs[o])) {
				return -EINVAL;   /* Stage A: one CRTC per commit */
			}
			out->crtc_named = 1;
			out->crtc_id = objs[o];
		}
		for (p = 0u; p < count_props[o]; p++, k++) {
			switch (kind) {
				case DRMPHX_OBJ_PLANE:
					rc = plane_slot(out, objs[o], baseline, ctx, &st, &idx);
					if (rc == 0) {
						rc = set_plane_prop(out, idx, st, props[k], values[k]);
					}
					break;
				case DRMPHX_OBJ_CRTC:
					rc = set_crtc_prop(out, props[k], values[k]);
					break;
				case DRMPHX_OBJ_CONNECTOR:
					rc = set_connector_prop(out, props[k], values[k]);
					break;
				default:
					rc = -ENOENT;
					break;
			}
			if (rc != 0) {
				return rc;
			}
		}
		/* An object listed with zero properties still names its plane. */
		if ((kind == DRMPHX_OBJ_PLANE) && (count_props[o] == 0u)) {
			rc = plane_slot(out, objs[o], baseline, ctx, &st, &idx);
			if (rc != 0) {
				return rc;
			}
		}
	}

	/* The commit's CRTC: named, else the first plane's, else display 0. */
	if (out->crtc_id == 0u) {
		for (i = 0u; i < out->nplanes; i++) {
			if (out->st[i].crtc_id != 0u) {
				out->crtc_id = out->st[i].crtc_id;
				break;
			}
		}
	}
	if (out->crtc_id == 0u) {
		out->crtc_id = KMS_ID_CRTC(0u);
	}
	for (i = 0u; i < out->nplanes; i++) {
		if ((out->st[i].crtc_id != 0u) && (out->st[i].crtc_id != out->crtc_id)) {
			return -EINVAL;   /* Stage A: every plane of a commit on the one CRTC */
		}
	}
	return 0;
}


static void copy_user(uint64_t uptr, const void *src, size_t n)
{
	if ((uptr != 0u) && (n != 0u)) {
		memcpy((void *)(uintptr_t)uptr, src, n);
	}
}


void drmphx_fill_property(const kms_property_t *p, const void *data, size_t data_size, struct drm_mode_get_property *gp)
{
	uint32_t cap_values = gp->count_values, cap_enums = gp->count_enum_blobs, i, n;
	const kms_prop_enum_t *en = data;
	const uint64_t *vals = data;
	struct drm_mode_property_enum e;
	uint64_t v;

	memset(gp->name, 0, sizeof(gp->name));
	memcpy(gp->name, p->name, (sizeof(gp->name) < sizeof(p->name)) ? sizeof(gp->name) : sizeof(p->name));
	gp->name[sizeof(gp->name) - 1u] = '\0';
	gp->flags = p->flags;

	if ((p->flags & (KMS_PROP_FLAG_ENUM | KMS_PROP_FLAG_BITMASK)) != 0u) {
		/* DRM: values[i] = enum value, enum_blob[i] = {value, name}; both counts = n */
		n = p->nenums;
		for (i = 0u; (i < n) && ((i + 1u) * sizeof(kms_prop_enum_t) <= data_size); i++) {
			v = en[i].value;
			if (i < cap_values) {
				copy_user(gp->values_ptr + i * sizeof(uint64_t), &v, sizeof(v));
			}
			if (i < cap_enums) {
				memset(&e, 0, sizeof(e));
				e.value = v;
				memcpy(e.name, en[i].name, (sizeof(e.name) < sizeof(en[i].name)) ? sizeof(e.name) : sizeof(en[i].name));
				e.name[sizeof(e.name) - 1u] = '\0';
				copy_user(gp->enum_blob_ptr + i * sizeof(e), &e, sizeof(e));
			}
		}
		gp->count_values = n;
		gp->count_enum_blobs = n;
		return;
	}
	if ((p->flags & KMS_PROP_FLAG_BLOB) != 0u) {
		gp->count_values = 0u;   /* modern DRM lists no blob ids here */
		gp->count_enum_blobs = 0u;
		return;
	}
	/* range (2: min, max), signed range (2), object (1: the object type) */
	n = p->nvalues;
	for (i = 0u; (i < n) && (i < cap_values) && ((i + 1u) * sizeof(uint64_t) <= data_size); i++) {
		copy_user(gp->values_ptr + i * sizeof(uint64_t), &vals[i], sizeof(uint64_t));
	}
	gp->count_values = n;
	gp->count_enum_blobs = 0u;
}


static void fill_str(size_t *len, char *dst, const char *src)
{
	size_t n = strlen(src);

	if ((dst != NULL) && (*len != 0u)) {
		memcpy(dst, src, (n < *len) ? n : *len);
	}
	*len = n;
}


void drmphx_fill_version(struct drm_version *v, int major, int minor, int patch, const char *name, const char *date,
	const char *desc)
{
	v->version_major = major;
	v->version_minor = minor;
	v->version_patchlevel = patch;
	fill_str(&v->name_len, v->name, name);
	fill_str(&v->date_len, v->date, date);
	fill_str(&v->desc_len, v->desc, desc);
}


#define ALIGN_UP(x, a) (((x) + (a) - 1u) & ~((size_t)(a) - 1u))

drmDevicePtr drmphx_device_new(const char *fullname, const char *const *compatible,
	const char *const nodes[DRM_NODE_MAX])
{
	size_t node_len = ALIGN_UP((size_t)DRMPHX_NODE_PATH_MAX, sizeof(void *)), size, ncompat = 0u, i;
	drmDevicePtr dev;
	char *ptr;
	char **compat;

	size = sizeof(*dev) + DRM_NODE_MAX * (sizeof(void *) + node_len) + sizeof(drmPlatformBusInfo) +
		sizeof(drmPlatformDeviceInfo);
	dev = calloc(1, size);
	if (dev == NULL) {
		return NULL;
	}
	ptr = (char *)dev + sizeof(*dev);
	dev->nodes = (char **)(void *)ptr;
	ptr += DRM_NODE_MAX * sizeof(void *);
	for (i = 0u; i < DRM_NODE_MAX; i++) {
		dev->nodes[i] = ptr;
		ptr += node_len;
		if (nodes[i] != NULL) {
			strncpy(dev->nodes[i], nodes[i], node_len - 1u);
			dev->available_nodes |= 1 << i;
		}
	}
	dev->bustype = DRM_BUS_PLATFORM;
	dev->businfo.platform = (drmPlatformBusInfoPtr)(void *)ptr;
	strncpy(dev->businfo.platform->fullname, fullname, DRM_PLATFORM_DEVICE_NAME_LEN - 1u);
	ptr += sizeof(drmPlatformBusInfo);
	dev->deviceinfo.platform = (drmPlatformDeviceInfoPtr)(void *)ptr;

	while ((compatible != NULL) && (compatible[ncompat] != NULL)) {
		ncompat++;
	}
	compat = calloc(ncompat + 1u, sizeof(char *));
	if (compat == NULL) {
		free(dev);
		return NULL;
	}
	for (i = 0u; i < ncompat; i++) {
		compat[i] = strdup(compatible[i]);
		if (compat[i] == NULL) {
			while (i > 0u) {
				free(compat[--i]);
			}
			free(compat);
			free(dev);
			return NULL;
		}
	}
	dev->deviceinfo.platform->compatible = compat;
	return dev;
}
