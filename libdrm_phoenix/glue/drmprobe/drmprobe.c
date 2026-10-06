/*
 * Phoenix-RTOS
 *
 * drmprobe - end-to-end probe of libdrm-phoenix against rpi4-kms + rpi4-v3d-async
 *
 * Uses only the upstream libdrm API (xf86drm.h, xf86drmMode.h) and DRM ioctls,
 * as a Linux DRM program would; buffers are mapped with plain mmap(), which the
 * link routes through libdrm-phoenix (-Wl,--wrap=mmap). Every result is one
 * tagged line `DRMPROBE <test> ... ok=0|1` (or `gap=1`: a documented server gap
 * answered as predicted), and the run ends with `DRMPROBE RESULT ... verdict=`.
 *
 * Needs: rpi4-v3d-async running, then rpi4-kms (started with -G for the in-fence
 * test). Neither server may share the hardware with an old-lane GPU app.
 *
 * Usage: drmprobe [-n flips] [-k] [-g jobs]
 *   -k: keep the last frame on screen at exit
 *   -g: full-screen clears the G6 producer chains (default 48; the longer, the wider the race)
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
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "v3d_drm.h"
#include "v3da_clgen.h"

/* linux/sync_file.h, by layout (Mesa util/libsync.h carries the same copy). */
struct probe_sync_merge_data {
	char name[32];
	int32_t fd2;
	int32_t fence;
	uint32_t flags;
	uint32_t pad;
};
struct probe_sync_file_info {
	char name[32];
	int32_t status;
	uint32_t flags;
	uint32_t num_fences;
	uint32_t pad;
	uint64_t sync_fence_info;
};
#define PROBE_SYNC_IOC_MERGE     _IOWR('>', 3, struct probe_sync_merge_data)
#define PROBE_SYNC_IOC_FILE_INFO _IOWR('>', 4, struct probe_sync_file_info)

/* linux/dma-buf.h, by layout (Linux 6.0: implicit fences of a dma-buf as sync files). */
struct probe_dma_buf_sync_file {
	uint32_t flags;
	int32_t fd;
};
#define PROBE_DMA_BUF_SYNC_RW                (1u | 2u)
#define PROBE_DMA_BUF_IOCTL_EXPORT_SYNC_FILE _IOWR('b', 2, struct probe_dma_buf_sync_file)
#define PROBE_DMA_BUF_IOCTL_IMPORT_SYNC_FILE _IOW('b', 3, struct probe_dma_buf_sync_file)


extern int sys_fdpath(int fd, char *buf, size_t size);

#define TAG "DRMPROBE "

static struct {
	int pass, fail, gap;
	char failed[512];
	int card, render, card1;
	char card_path[64], render_path[64];
	uint32_t crtc, conn, primary;
	drmModeModeInfo mode;
	uint32_t fmt;
	struct {
		uint32_t handle, pitch, fb;
		uint64_t size;
		uint32_t *px;
	} buf[2];
	int flips_done;
	uint64_t last_flip_seq;
	uint64_t last_flip_us;
	uint32_t last_flip_crtc;
	uint32_t g6_jobs;         /* -g: jobs in the G6 producer's chain */
} P = { .card = -1, .render = -1, .card1 = -1, .g6_jobs = 48u };


static uint64_t now_us(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}


static void verdict(const char *key, int ok)
{
	if (ok) {
		P.pass++;
	}
	else {
		P.fail++;
		if (strlen(P.failed) + strlen(key) + 2 < sizeof(P.failed)) {
			strcat(P.failed, key);
			strcat(P.failed, ",");
		}
	}
}


/* A result predicted to hit a documented server gap: counts as `gap`, not fail,
 * when the error is the predicted one. */
static void gapcheck(const char *key, int rc_errno, int expect)
{
	if (rc_errno == expect) {
		P.gap++;
	}
	else if (rc_errno == 0) {
		P.pass++;   /* the gap was closed meanwhile */
	}
	else {
		verdict(key, 0);
	}
}


/* ========================================================================= */
/* 1. identity                                                                */
/* ========================================================================= */

static const char *node_or_dash(drmDevicePtr d, int t)
{
	return ((d->available_nodes & (1 << t)) != 0) ? d->nodes[t] : "-";
}


static void t_devices(char *card_path, char *render_path, char *card1_path, size_t n)
{
	drmDevicePtr devs[8];
	int cnt, i, cnt0;

	card_path[0] = render_path[0] = card1_path[0] = '\0';
	cnt0 = drmGetDevices2(0, NULL, 0);
	cnt = drmGetDevices2(0, devs, 8);
	printf(TAG "devices count_null=%d n=%d ok=%d\n", cnt0, cnt, (cnt == 2) && (cnt0 == 2));
	verdict("devices", (cnt == 2) && (cnt0 == 2));
	for (i = 0; i < cnt; i++) {
		drmDevicePtr d = devs[i];
		const char *compat = ((d->bustype == DRM_BUS_PLATFORM) && (d->deviceinfo.platform->compatible != NULL) &&
			(d->deviceinfo.platform->compatible[0] != NULL)) ? d->deviceinfo.platform->compatible[0] : "-";
		printf(TAG "device i=%d bus=%s fullname=%s compat=%s primary=%s render=%s\n", i,
			(d->bustype == DRM_BUS_PLATFORM) ? "platform" : "other",
			(d->bustype == DRM_BUS_PLATFORM) ? d->businfo.platform->fullname : "-", compat,
			node_or_dash(d, DRM_NODE_PRIMARY), node_or_dash(d, DRM_NODE_RENDER));
		if ((d->available_nodes & (1 << DRM_NODE_RENDER)) != 0) {
			snprintf(render_path, n, "%s", d->nodes[DRM_NODE_RENDER]);
			if ((d->available_nodes & (1 << DRM_NODE_PRIMARY)) != 0) {
				snprintf(card1_path, n, "%s", d->nodes[DRM_NODE_PRIMARY]);   /* the v3d device's primary node */
			}
		}
		else if ((d->available_nodes & (1 << DRM_NODE_PRIMARY)) != 0) {
			snprintf(card_path, n, "%s", d->nodes[DRM_NODE_PRIMARY]);
		}
	}
	drmFreeDevices(devs, (cnt > 0) ? cnt : 0);
}


/* Open exactly as Mesa does (O_RDWR | O_CLOEXEC); report, then fall back. */
static int t_open(const char *what, const char *path)
{
	int fd, rdwr = 1, e = 0;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		e = errno;
		rdwr = 0;
		fd = open(path, O_RDONLY | O_CLOEXEC);
	}
	printf(TAG "open node=%s path=%s rdwr=%d rdwr_errno=%d fd=%d ok=%d\n", what, path, rdwr, e, fd, rdwr && (fd >= 0));
	verdict(what, rdwr && (fd >= 0));
	return fd;
}


static void t_identity(const char *what, int fd, const char *want_name, int want_type, int want_kms)
{
	drmVersionPtr v = drmGetVersion(fd);
	drmDevicePtr d = NULL;
	char *n2 = drmGetDeviceNameFromFd2(fd), *rn = drmGetRenderDeviceNameFromFd(fd),
		*pn = drmGetPrimaryDeviceNameFromFd(fd);
	int type = drmGetNodeTypeFromFd(fd), kms = drmIsKMS(fd), dev_rc = drmGetDevice2(fd, 0, &d), ok;

	ok = (v != NULL) && (strcmp(v->name, want_name) == 0) && (type == want_type) && (kms == want_kms) && (dev_rc == 0) &&
		(n2 != NULL);
	printf(TAG "identity node=%s version=%s %d.%d.%d date=%s desc=\"%s\" node_type=%d is_kms=%d name2=%s "
		"primary_name=%s render_name=%s get_device=%d dev_primary=%s dev_render=%s ok=%d\n", what,
		(v != NULL) ? v->name : "-", (v != NULL) ? v->version_major : -1, (v != NULL) ? v->version_minor : -1,
		(v != NULL) ? v->version_patchlevel : -1, (v != NULL) ? v->date : "-", (v != NULL) ? v->desc : "-", type, kms,
		(n2 != NULL) ? n2 : "-", (pn != NULL) ? pn : "-", (rn != NULL) ? rn : "-", dev_rc,
		(d != NULL) ? node_or_dash(d, DRM_NODE_PRIMARY) : "-", (d != NULL) ? node_or_dash(d, DRM_NODE_RENDER) : "-", ok);
	verdict(what, ok);
	drmFreeVersion(v);
	drmFreeDevice(&d);
	free(n2);
	free(rn);
	free(pn);
}


/* G2 + G10 (M3 part 2): fstat() on every node - Mesa's gbm_create_device()
 * needs S_ISCHR, v3dv compares the primary and render st_rdev - and the dev_t
 * maps back to the right device and node type through libdrm. */
static void t_fstat(void)
{
	const struct {
		const char *what;
		int fd;
		int type;
	} n[3] = { { "card0", P.card, DRM_NODE_PRIMARY }, { "card1", P.card1, DRM_NODE_PRIMARY },
		{ "render", P.render, DRM_NODE_RENDER } };
	struct stat st[3];
	int rc[3] = { -1, -1, -1 }, i, nopen = 0, nok = 0, nsys = 0, chr_all = 1, types_ok = 1, distinct, ok;

	memset(st, 0, sizeof(st));
	for (i = 0; i < 3; i++) {
		drmDevicePtr d = NULL;
		int e, t = -1, drc = -1;
		if (n[i].fd < 0) {
			continue;
		}
		nopen++;
		rc[i] = fstat(n[i].fd, &st[i]);
		e = (rc[i] != 0) ? errno : 0;
		if (rc[i] == 0) {
			nok++;
			chr_all &= S_ISCHR(st[i].st_mode) ? 1 : 0;
			t = drmGetNodeTypeFromDevId(st[i].st_rdev);
			drc = drmGetDeviceFromDevId(st[i].st_rdev, 0, &d);
			types_ok &= ((t == n[i].type) && (drc == 0)) ? 1 : 0;
		}
		else {
			nsys += (e == ENOSYS) ? 1 : 0;
		}
		printf(TAG "fstat node=%s fd=%d rc=%d errno=%d mode=0%o chr=%d rdev=%llu devid_type=%d devid_dev=%d\n", n[i].what,
			n[i].fd, rc[i], e, (unsigned)st[i].st_mode, (rc[i] == 0) ? (S_ISCHR(st[i].st_mode) ? 1 : 0) : 0,
			(unsigned long long)st[i].st_rdev, t, drc);
		drmFreeDevice(&d);
	}
	/* card0 != render always; card1 (when its own name exists) differs from both */
	distinct = (rc[0] == 0) && (rc[2] == 0) && (st[0].st_rdev != st[2].st_rdev) &&
		((P.card1 < 0) || ((rc[1] == 0) && (st[1].st_rdev != st[2].st_rdev) && (st[1].st_rdev != st[0].st_rdev)));
	ok = (nok == nopen) && chr_all && types_ok && distinct;
	printf(TAG "fstat_nodes n=%d answered=%d chr_all=%d distinct=%d devid_ok=%d ok=%d gap=%d\n", nopen, nok, chr_all,
		distinct, types_ok, ok, (nsys == nopen) ? 1 : 0);
	if ((nsys == nopen) && (nopen > 0)) {
		P.gap++;   /* every server answered -ENOSYS: servers from before M3 part 2 (G2) */
	}
	else {
		verdict("fstat", ok);
	}
}


/* ========================================================================= */
/* 2. KMS enumeration                                                         */
/* ========================================================================= */

static const char *prop_name_of(int fd, uint32_t prop, char *buf, size_t n)
{
	drmModePropertyPtr p = drmModeGetProperty(fd, prop);

	snprintf(buf, n, "%s", (p != NULL) ? p->name : "?");
	drmModeFreeProperty(p);
	return buf;
}


/* M5: DRM's SET_CLIENT_CAP(ATOMIC) also turns on universal planes. Atomic-only
 * clients -- Mesa's VK_KHR_display WSI sets ATOMIC and never UNIVERSAL_PLANES --
 * rely on it to see the primary plane. A fresh card0 open, ATOMIC only. */
static void t_atomic_universal(const char *card_path)
{
	drmModePlaneResPtr pr;
	drmModeObjectPropertiesPtr props;
	drmModePropertyPtr prop;
	uint32_t i, j;
	int fd, rc_a = -1, n = -1, primary = 0, ok;

	fd = open(card_path, O_RDWR | O_CLOEXEC);
	if (fd >= 0) {
		rc_a = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
		pr = drmModeGetPlaneResources(fd);
		if (pr != NULL) {
			n = (int)pr->count_planes;
			for (i = 0u; i < pr->count_planes; i++) {
				props = drmModeObjectGetProperties(fd, pr->planes[i], DRM_MODE_OBJECT_PLANE);
				for (j = 0u; (props != NULL) && (j < props->count_props); j++) {
					prop = drmModeGetProperty(fd, props->props[j]);
					if ((prop != NULL) && (strcmp(prop->name, "type") == 0) &&
							(props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)) {
						primary++;
					}
					drmModeFreeProperty(prop);
				}
				drmModeFreeObjectProperties(props);
			}
			drmModeFreePlaneResources(pr);
		}
		close(fd);
	}
	ok = (rc_a == 0) && (n >= 1) && (primary == 1);
	printf(TAG "atomic_universal open=%d atomic=%d planes=%d primary=%d ok=%d\n", fd >= 0, rc_a, n, primary, ok);
	verdict("atomic_universal", ok);
}


static int t_kms_enum(void)
{
	drmModeResPtr res;
	drmModeConnectorPtr conn = NULL;
	drmModeEncoderPtr enc = NULL;
	drmModeCrtcPtr crtc = NULL;
	drmModePlaneResPtr pres;
	drmModeObjectPropertiesPtr props;
	uint64_t cap_dumb = 0, cap_prime = 0, cap_mono = 0, cap_invbl = 0, cap_async = 9;
	int rc_u, rc_a, ok, i, j, nprops_total = 0, enum_ok = 1;
	char nb[40];

	rc_u = drmSetClientCap(P.card, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	rc_a = drmSetClientCap(P.card, DRM_CLIENT_CAP_ATOMIC, 1);
	(void)drmGetCap(P.card, DRM_CAP_DUMB_BUFFER, &cap_dumb);
	(void)drmGetCap(P.card, DRM_CAP_PRIME, &cap_prime);
	(void)drmGetCap(P.card, DRM_CAP_TIMESTAMP_MONOTONIC, &cap_mono);
	(void)drmGetCap(P.card, DRM_CAP_CRTC_IN_VBLANK_EVENT, &cap_invbl);
	(void)drmGetCap(P.card, DRM_CAP_ASYNC_PAGE_FLIP, &cap_async);
	ok = (rc_u == 0) && (rc_a == 0) && (cap_dumb == 1u) && (cap_prime == 3u) && (cap_mono == 1u);
	printf(TAG "caps universal=%d atomic=%d dumb=%llu prime=%llu monotonic=%llu crtc_in_vblank=%llu async_flip=%llu ok=%d\n",
		rc_u, rc_a, (unsigned long long)cap_dumb, (unsigned long long)cap_prime, (unsigned long long)cap_mono,
		(unsigned long long)cap_invbl, (unsigned long long)cap_async, ok);
	verdict("caps", ok);

	res = drmModeGetResources(P.card);
	if (res == NULL) {
		printf(TAG "resources errno=%d ok=0\n", errno);
		verdict("resources", 0);
		return -1;
	}
	printf(TAG "resources crtcs=%d connectors=%d encoders=%d fbs=%d min=%ux%u max=%ux%u ok=%d\n", res->count_crtcs,
		res->count_connectors, res->count_encoders, res->count_fbs, res->min_width, res->min_height, res->max_width,
		res->max_height, (res->count_crtcs >= 1) && (res->count_connectors >= 1));
	verdict("resources", (res->count_crtcs >= 1) && (res->count_connectors >= 1));

	if (res->count_connectors >= 1) {
		conn = drmModeGetConnector(P.card, res->connectors[0]);
	}
	ok = (conn != NULL) && (conn->count_modes >= 1) && (conn->connection == DRM_MODE_CONNECTED);
	if (conn != NULL) {
		printf(TAG "connector id=0x%x type=%u type_id=%u connection=%u mm=%ux%u modes=%d mode0=%s %ux%u@%u clock=%u "
			"encoder=0x%x props=%d ok=%d\n", conn->connector_id, conn->connector_type, conn->connector_type_id,
			conn->connection, conn->mmWidth, conn->mmHeight, conn->count_modes,
			(conn->count_modes > 0) ? conn->modes[0].name : "-", (conn->count_modes > 0) ? conn->modes[0].hdisplay : 0,
			(conn->count_modes > 0) ? conn->modes[0].vdisplay : 0, (conn->count_modes > 0) ? conn->modes[0].vrefresh : 0,
			(conn->count_modes > 0) ? conn->modes[0].clock : 0, conn->encoder_id, conn->count_props, ok);
	}
	else {
		printf(TAG "connector errno=%d ok=0\n", errno);
	}
	verdict("connector", ok);
	if (!ok) {
		drmModeFreeConnector(conn);
		drmModeFreeResources(res);
		return -1;
	}
	P.conn = conn->connector_id;
	P.mode = conn->modes[0];

	enc = drmModeGetEncoder(P.card, conn->encoder_id);
	ok = (enc != NULL) && (enc->crtc_id != 0u);
	printf(TAG "encoder id=0x%x type=%u crtc=0x%x possible_crtcs=0x%x ok=%d\n", conn->encoder_id,
		(enc != NULL) ? enc->encoder_type : 0u, (enc != NULL) ? enc->crtc_id : 0u,
		(enc != NULL) ? enc->possible_crtcs : 0u, ok);
	verdict("encoder", ok);
	P.crtc = (enc != NULL) ? enc->crtc_id : res->crtcs[0];

	crtc = drmModeGetCrtc(P.card, P.crtc);
	ok = (crtc != NULL) && (crtc->mode_valid != 0) && (crtc->mode.hdisplay == P.mode.hdisplay);
	printf(TAG "crtc id=0x%x fb=%u mode_valid=%d mode=%ux%u@%u gamma=%d ok=%d\n", P.crtc,
		(crtc != NULL) ? crtc->buffer_id : 0u, (crtc != NULL) ? crtc->mode_valid : 0,
		(crtc != NULL) ? crtc->mode.hdisplay : 0, (crtc != NULL) ? crtc->mode.vdisplay : 0,
		(crtc != NULL) ? crtc->mode.vrefresh : 0, (crtc != NULL) ? crtc->gamma_size : -1, ok);
	verdict("crtc", ok);

	pres = drmModeGetPlaneResources(P.card);
	ok = (pres != NULL) && (pres->count_planes >= 1);
	printf(TAG "planes n=%d ok=%d\n", (pres != NULL) ? (int)pres->count_planes : -1, ok);
	verdict("planes", ok);
	P.fmt = DRM_FORMAT_XRGB8888;
	for (i = 0; (pres != NULL) && (i < (int)pres->count_planes); i++) {
		drmModePlanePtr pl = drmModeGetPlane(P.card, pres->planes[i]);
		uint64_t type = 99;
		props = drmModeObjectGetProperties(P.card, pres->planes[i], DRM_MODE_OBJECT_PLANE);
		for (j = 0; (props != NULL) && (j < (int)props->count_props); j++) {
			drmModePropertyPtr p = drmModeGetProperty(P.card, props->props[j]);
			if (p == NULL) {
				enum_ok = 0;
				continue;
			}
			if (strcmp(p->name, "type") == 0) {
				type = props->prop_values[j];
				if (((p->flags & DRM_MODE_PROP_ENUM) == 0) || (p->count_enums != 3)) {
					enum_ok = 0;
				}
			}
			nprops_total++;
			drmModeFreeProperty(p);
		}
		printf(TAG "plane id=0x%x type=%llu crtc=0x%x fb=%u formats=%u fmt0=0x%08x props=%d\n", pres->planes[i],
			(unsigned long long)type, (pl != NULL) ? pl->crtc_id : 0u, (pl != NULL) ? pl->fb_id : 0u,
			(pl != NULL) ? pl->count_formats : 0u, ((pl != NULL) && (pl->count_formats > 0)) ? pl->formats[0] : 0u,
			(props != NULL) ? (int)props->count_props : -1);
		if (type == DRM_PLANE_TYPE_PRIMARY) {
			int has_xrgb = 0;
			P.primary = pres->planes[i];
			for (j = 0; (pl != NULL) && (j < (int)pl->count_formats); j++) {
				has_xrgb |= (pl->formats[j] == DRM_FORMAT_XRGB8888);
			}
			if (!has_xrgb && (pl != NULL) && (pl->count_formats > 0)) {
				P.fmt = pl->formats[0];   /* pan backend: the firmware fb's own format */
			}
		}
		drmModeFreeObjectProperties(props);
		drmModeFreePlane(pl);
	}
	props = drmModeObjectGetProperties(P.card, P.crtc, DRM_MODE_OBJECT_CRTC);
	for (j = 0; (props != NULL) && (j < (int)props->count_props); j++) {
		printf(TAG "crtc_prop %s=%llu\n", prop_name_of(P.card, props->props[j], nb, sizeof(nb)),
			(unsigned long long)props->prop_values[j]);
	}
	drmModeFreeObjectProperties(props);
	ok = (P.primary != 0u) && enum_ok && (nprops_total > 0);
	printf(TAG "properties primary=0x%x total=%d enum_ok=%d fmt=0x%08x ok=%d\n", P.primary, nprops_total, enum_ok, P.fmt,
		ok);
	verdict("properties", ok);

	drmModeFreePlaneResources(pres);
	drmModeFreeCrtc(crtc);
	drmModeFreeEncoder(enc);
	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	return 0;
}


/* ========================================================================= */
/* 3. dumb buffers, flips, events                                             */
/* ========================================================================= */

static uint32_t rgb(uint32_t r, uint32_t g, uint32_t b)
{
	if ((P.fmt == DRM_FORMAT_XBGR8888) || (P.fmt == DRM_FORMAT_ABGR8888)) {
		return 0xff000000u | (b << 16) | (g << 8) | r;
	}
	return 0xff000000u | (r << 16) | (g << 8) | b;
}


/* Buffer i: a background colour per buffer (drawn once, full frame) and a white
 * 128x128 square whose position follows the frame number k. Per frame only the
 * square moves (erase + draw): a full 1080p redraw into uncached memory costs
 * 10-60 ms (M2 section 13) and would pace the flip test instead of the vblank. */
static uint32_t sq_x[2] = { ~0u, ~0u };

static void fill(int i, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t colour)
{
	uint32_t stride = P.buf[i].pitch / 4u, x, y;
	volatile uint32_t *px = P.buf[i].px;

	for (y = y0; y < y0 + h; y++) {
		for (x = x0; x < x0 + w; x++) {
			px[y * stride + x] = colour;
		}
	}
}


static uint32_t bg_of(int i)
{
	return (i == 0) ? rgb(0, 96, 128) : rgb(128, 0, 96);
}


static void draw(int i, int k)
{
	uint32_t w = P.mode.hdisplay, h = P.mode.vdisplay, x0 = (uint32_t)(k * 16) % (w - 128u), y0 = h / 2u - 64u;

	if (sq_x[i] == ~0u) {
		fill(i, 0, 0, w, h, bg_of(i));
	}
	else {
		fill(i, sq_x[i], y0, 128u, 128u, bg_of(i));
	}
	fill(i, x0, y0, 128u, 128u, rgb(255, 255, 255));
	sq_x[i] = x0;
}


static void on_flip(int fd, unsigned int seq, unsigned int sec, unsigned int usec, void *user)
{
	(void)fd;
	(void)sec;
	(void)usec;
	(void)user;
	P.flips_done++;
	P.last_flip_seq = seq;
	P.last_flip_us = now_us();
}


static void on_vblank(int fd, unsigned int seq, unsigned int sec, unsigned int usec, void *user)
{
	on_flip(fd, seq, sec, usec, user);
}


/* Block until `target` flip events arrived (drmHandleEvent read()s the card fd;
 * the server parks the read <= 2 s, then -EAGAIN). Returns 0 or -ETIMEDOUT. */
static int wait_flips(int target, unsigned timeout_ms)
{
	drmEventContext ev;
	uint64_t t0 = now_us();

	memset(&ev, 0, sizeof(ev));
	ev.version = 2;
	ev.page_flip_handler = on_flip;
	ev.vblank_handler = on_vblank;
	while (P.flips_done < target) {
		if ((drmHandleEvent(P.card, &ev) < 0) && (errno != EAGAIN) && (errno != EINTR)) {
			return -errno;
		}
		if ((now_us() - t0) > (uint64_t)timeout_ms * 1000u) {
			return -ETIMEDOUT;
		}
	}
	return 0;
}


static int t_dumb(void)
{
	int i, ok;
	void *p;
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	uint64_t off;

	for (i = 0; i < 2; i++) {
		int rc = drmModeCreateDumbBuffer(P.card, P.mode.hdisplay, P.mode.vdisplay, 32, 0, &P.buf[i].handle,
			&P.buf[i].pitch, &P.buf[i].size);
		int mrc = (rc == 0) ? drmModeMapDumbBuffer(P.card, P.buf[i].handle, &off) : -1;
		p = (mrc == 0) ? mmap(NULL, (size_t)P.buf[i].size, PROT_READ | PROT_WRITE, MAP_SHARED, P.card, (off_t)off) : MAP_FAILED;
		P.buf[i].px = (p != MAP_FAILED) ? p : NULL;
		ok = (rc == 0) && (mrc == 0) && (P.buf[i].px != NULL);
		if (ok) {
			draw(i, 0);
			ok = (P.buf[i].px[0] == bg_of(i));
		}
		handles[0] = P.buf[i].handle;
		pitches[0] = P.buf[i].pitch;
		if (ok) {
			ok = (drmModeAddFB2(P.card, P.mode.hdisplay, P.mode.vdisplay, P.fmt, handles, pitches, offsets,
				&P.buf[i].fb, 0) == 0);
		}
		printf(TAG "dumb i=%d create=%d handle=%u pitch=%u size=%llu map=%d token=0x%llx mmap=%s readback=%d fb=%u ok=%d\n",
			i, rc, P.buf[i].handle, P.buf[i].pitch, (unsigned long long)P.buf[i].size, mrc, (unsigned long long)off,
			(P.buf[i].px != NULL) ? "ok" : strerror(errno), (P.buf[i].px != NULL) ? 1 : 0, P.buf[i].fb, ok);
		verdict((i == 0) ? "dumb0" : "dumb1", ok);
		if (!ok) {
			return -1;
		}
	}
	return 0;
}


static void t_flips(int nflips)
{
	uint64_t t0, t1, seq0;
	int rc, i, missed = 0, ok;
	drmVBlank vb;
	uint64_t cseq = 0, cns = 0;

	t0 = now_us();
	rc = drmModeSetCrtc(P.card, P.crtc, P.buf[0].fb, 0, 0, &P.conn, 1, &P.mode);
	t1 = now_us();
	printf(TAG "setcrtc rc=%d errno=%d blocking_ms=%llu ok=%d\n", rc, (rc != 0) ? errno : 0,
		(unsigned long long)((t1 - t0) / 1000u), rc == 0);
	verdict("setcrtc", rc == 0);
	if (rc != 0) {
		return;
	}

	/* one flip with an event: latency of the event path */
	P.flips_done = 0;
	draw(1, 1);
	t0 = now_us();
	rc = drmModePageFlip(P.card, P.crtc, P.buf[1].fb, DRM_MODE_PAGE_FLIP_EVENT, (void *)(uintptr_t)0x1234);
	if (rc == 0) {
		rc = wait_flips(1, 3000);
	}
	ok = (rc == 0) && (P.flips_done == 1);
	printf(TAG "flip_event rc=%d events=%d seq=%llu latency_us=%llu ok=%d\n", rc, P.flips_done,
		(unsigned long long)P.last_flip_seq, (unsigned long long)(P.last_flip_us - t0), ok);
	verdict("flip_event", ok);
	if (!ok) {
		return;
	}

	/* a run of vsynced flips, each waiting for its event */
	seq0 = P.last_flip_seq;
	P.flips_done = 0;
	t0 = now_us();
	for (i = 0; i < nflips; i++) {
		int b = i & 1;
		draw(b, i + 2);
		rc = drmModePageFlip(P.card, P.crtc, P.buf[b].fb, DRM_MODE_PAGE_FLIP_EVENT, NULL);
		if (rc != 0) {
			break;
		}
		rc = wait_flips(i + 1, 3000);
		if (rc != 0) {
			break;
		}
	}
	t1 = now_us();
	missed = (int)(P.last_flip_seq - seq0) - P.flips_done;
	ok = (rc == 0) && (P.flips_done == nflips);
	printf(TAG "flips n=%d done=%d rc=%d ms=%llu fps_x100=%llu vblanks=%llu missed_vblanks=%d ok=%d\n", nflips,
		P.flips_done, rc, (unsigned long long)((t1 - t0) / 1000u),
		(unsigned long long)((t1 > t0) ? ((uint64_t)P.flips_done * 100000000u / (t1 - t0)) : 0u),
		(unsigned long long)(P.last_flip_seq - seq0), missed, ok);
	verdict("flips", ok);

	/* drmWaitVBlank (relative 1, blocking) and the sequence API */
	memset(&vb, 0, sizeof(vb));
	vb.request.type = DRM_VBLANK_RELATIVE;
	vb.request.sequence = 1;
	t0 = now_us();
	rc = drmWaitVBlank(P.card, &vb);
	t1 = now_us();
	i = drmCrtcGetSequence(P.card, P.crtc, &cseq, &cns);
	ok = (rc == 0) && (i == 0) && (cseq >= vb.reply.sequence) && ((t1 - t0) < 40000u);
	printf(TAG "vblank wait_rc=%d seq=%u waited_us=%llu tv=%ld.%06ld get_seq_rc=%d seq64=%llu ok=%d\n", rc,
		vb.reply.sequence, (unsigned long long)(t1 - t0), vb.reply.tval_sec, vb.reply.tval_usec, i,
		(unsigned long long)cseq, ok);
	verdict("vblank", ok);

	/* poll() readiness on the card fd (quantised to 20 ms until the kernel block_ms change, E5) */
	{
		struct pollfd pfd = { .fd = P.card, .events = POLLIN };
		P.flips_done = 0;
		rc = drmModePageFlip(P.card, P.crtc, P.buf[0].fb, DRM_MODE_PAGE_FLIP_EVENT, NULL);
		t0 = now_us();
		i = (rc == 0) ? poll(&pfd, 1, 1000) : -1;
		t1 = now_us();
		ok = (rc == 0) && (i == 1) && ((pfd.revents & POLLIN) != 0) && (wait_flips(1, 3000) == 0);
		printf(TAG "poll flip_rc=%d poll=%d revents=0x%x wake_us=%llu ok=%d\n", rc, i, pfd.revents,
			(unsigned long long)(t1 - t0), ok);
		verdict("poll", ok);
	}
}


/* Atomic: TEST_ONLY, then a real NONBLOCK commit with an event. */
static void t_atomic(void)
{
	drmModeObjectPropertiesPtr props;
	drmModeAtomicReqPtr req;
	uint32_t fb_prop = 0;
	int j, rc_t, rc_c = -1, ok;
	char nb[40];

	props = drmModeObjectGetProperties(P.card, P.primary, DRM_MODE_OBJECT_PLANE);
	for (j = 0; (props != NULL) && (j < (int)props->count_props); j++) {
		if (strcmp(prop_name_of(P.card, props->props[j], nb, sizeof(nb)), "FB_ID") == 0) {
			fb_prop = props->props[j];
		}
	}
	drmModeFreeObjectProperties(props);

	req = drmModeAtomicAlloc();
	(void)drmModeAtomicAddProperty(req, P.primary, fb_prop, P.buf[1].fb);
	rc_t = drmModeAtomicCommit(P.card, req, DRM_MODE_ATOMIC_TEST_ONLY, NULL);
	if (rc_t == 0) {
		P.flips_done = 0;
		rc_c = drmModeAtomicCommit(P.card, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL);
		if (rc_c == 0) {
			rc_c = wait_flips(1, 3000);
		}
	}
	drmModeAtomicFree(req);
	ok = (fb_prop != 0u) && (rc_t == 0) && (rc_c == 0);
	printf(TAG "atomic fb_prop=0x%x test_only=%d commit=%d events=%d ok=%d\n", fb_prop, rc_t, rc_c, P.flips_done, ok);
	verdict("atomic", ok);
}


/* ========================================================================= */
/* 4. render node: params, BOs, a CL clear, syncobjs, sync files              */
/* ========================================================================= */

typedef struct {
	uint32_t handle, offset, size;
	uint32_t *cpu;
} rbo_t;


/* Phoenix-only DRM_IOCTL_V3D_CREATE_BO flag (libdrm-phoenix drm_phoenix_ext.h
 * DRM_PHOENIX_V3D_CREATE_BO_SCANOUT; Mesa v3d sets it for PIPE_BIND_SCANOUT
 * resources on Phoenix): the BO may be scanned out - the render server (proto 5)
 * places it below 1 GiB. The probe uses only upstream headers, hence the copy. */
#define PROBE_V3D_CREATE_BO_SCANOUT (1u << 31)


static int rbo_new_flags_fd(int fd, rbo_t *b, uint32_t size, uint32_t flags)
{
	struct drm_v3d_create_bo cb = { .size = size, .flags = flags };
	struct drm_v3d_mmap_bo mb;
	void *p;

	memset(b, 0, sizeof(*b));
	if (drmIoctl(fd, DRM_IOCTL_V3D_CREATE_BO, &cb) != 0) {
		return -errno;
	}
	b->handle = cb.handle;
	b->offset = cb.offset;
	b->size = size;
	memset(&mb, 0, sizeof(mb));
	mb.handle = cb.handle;
	if (drmIoctl(fd, DRM_IOCTL_V3D_MMAP_BO, &mb) != 0) {
		return -errno;
	}
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)mb.offset);
	if (p == MAP_FAILED) {
		return -errno;
	}
	b->cpu = p;
	return 0;
}


static int rbo_new_fd(int fd, rbo_t *b, uint32_t size)
{
	return rbo_new_flags_fd(fd, b, size, 0u);
}


static void rbo_free_fd(int fd, rbo_t *b)
{
	struct drm_gem_close gc = { .handle = b->handle };

	if (b->cpu != NULL) {
		(void)munmap(b->cpu, b->size);
	}
	if (b->handle != 0u) {
		(void)drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
	}
	memset(b, 0, sizeof(*b));
}


static int rbo_new(rbo_t *b, uint32_t size)
{
	return rbo_new_fd(P.render, b, size);
}


static void rbo_free(rbo_t *b)
{
	rbo_free_fd(P.render, b);
}


static void t_render_basics(void)
{
	static const uint32_t params[] = { DRM_V3D_PARAM_V3D_CORE0_IDENT0, DRM_V3D_PARAM_V3D_HUB_IDENT1,
		DRM_V3D_PARAM_SUPPORTS_TFU, DRM_V3D_PARAM_SUPPORTS_CSD, DRM_V3D_PARAM_SUPPORTS_CACHE_FLUSH,
		DRM_V3D_PARAM_SUPPORTS_MULTISYNC_EXT, DRM_V3D_PARAM_SUPPORTS_CPU_QUEUE, DRM_V3D_PARAM_SUPPORTS_PERFMON };
	uint64_t v[8], cap_sync = 9, cap_tl = 9, cap_prime = 9;
	struct drm_v3d_get_param gp;
	struct drm_v3d_get_bo_offset go;
	struct drm_v3d_wait_bo wb;
	rbo_t b;
	unsigned i;
	int rc, ok, wrc;

	for (i = 0; i < 8u; i++) {
		memset(&gp, 0, sizeof(gp));
		gp.param = params[i];
		v[i] = (drmIoctl(P.render, DRM_IOCTL_V3D_GET_PARAM, &gp) == 0) ? gp.value : 0xdeadu;
	}
	ok = (v[0] == 0x04443356u) && (v[2] == 1u) && (v[3] == 1u) && (v[5] == 1u);
	printf(TAG "v3d_params ident0=0x%08llx hub_ident1=0x%08llx tfu=%llu csd=%llu cache_flush=%llu multisync=%llu "
		"cpu_queue=%llu perfmon=%llu ok=%d\n", (unsigned long long)v[0], (unsigned long long)v[1],
		(unsigned long long)v[2], (unsigned long long)v[3], (unsigned long long)v[4], (unsigned long long)v[5],
		(unsigned long long)v[6], (unsigned long long)v[7], ok);
	verdict("v3d_params", ok);

	(void)drmGetCap(P.render, DRM_CAP_SYNCOBJ, &cap_sync);
	(void)drmGetCap(P.render, DRM_CAP_SYNCOBJ_TIMELINE, &cap_tl);
	(void)drmGetCap(P.render, DRM_CAP_PRIME, &cap_prime);
	ok = (cap_sync == 1u) && (cap_tl == 0u);
	printf(TAG "render_caps syncobj=%llu timeline=%llu prime=%llu ok=%d\n", (unsigned long long)cap_sync,
		(unsigned long long)cap_tl, (unsigned long long)cap_prime, ok);
	verdict("render_caps", ok);

	rc = rbo_new(&b, 65536u);
	ok = (rc == 0);
	if (ok) {
		for (i = 0; i < 65536u / 4u; i++) {
			b.cpu[i] = 0xa5a50000u | i;
		}
		ok = (b.cpu[1234] == (0xa5a50000u | 1234u));
		memset(&go, 0, sizeof(go));
		go.handle = b.handle;
		ok = ok && (drmIoctl(P.render, DRM_IOCTL_V3D_GET_BO_OFFSET, &go) == 0) && (go.offset == b.offset);
	}
	memset(&wb, 0, sizeof(wb));
	wb.handle = b.handle;
	wrc = drmIoctl(P.render, DRM_IOCTL_V3D_WAIT_BO, &wb);
	printf(TAG "v3d_bo create=%d handle=0x%x gpuva=0x%x cpu=%p readback=%d wait_idle=%d ok=%d\n", rc, b.handle, b.offset,
		(void *)b.cpu, (b.cpu != NULL) ? 1 : 0, wrc, ok && (wrc == 0));
	verdict("v3d_bo", ok && (wrc == 0));
	rbo_free(&b);
}


/* The M1-proven clear (v3da_clgen.c): 64x64 RGBA8, zero draws. Returns the sync
 * handle of the job (or 0), leaves the RT mapped in *rt for a second check. */
static int cl_clear(rbo_t *rt, rbo_t *bcl, rbo_t *rcl, rbo_t *ta, rbo_t *ts, uint32_t colour, uint32_t in_sync,
	uint32_t *out_sync, int *pixels_ok, int wait)
{
	struct drm_v3d_submit_cl s;
	v3da_clgen_buf_t gb, gr;
	uint32_t handles[5], i, bad = 0;
	int rc;

	for (i = 0; i < 64u * 64u; i++) {
		rt->cpu[i] = 0xdeadbeefu;
	}
	gb.cpu = bcl->cpu;
	gb.gpuva = bcl->offset;
	gb.size = bcl->size;
	gr.cpu = rcl->cpu;
	gr.gpuva = rcl->offset;
	gr.size = rcl->size;
	rc = v3da_clgen_clear(64u, 64u, rt->offset, colour, &gb, &gr, ta->offset, ta->size, ts->offset, &s);
	if (rc != 0) {
		return rc;
	}
	handles[0] = rt->handle;
	handles[1] = bcl->handle;
	handles[2] = rcl->handle;
	handles[3] = ta->handle;
	handles[4] = ts->handle;
	s.bo_handles = (uintptr_t)handles;
	s.bo_handle_count = 5u;
	s.in_sync_bcl = in_sync;
	s.out_sync = *out_sync;
	if (drmIoctl(P.render, DRM_IOCTL_V3D_SUBMIT_CL, &s) != 0) {
		return -errno;
	}
	if (!wait) {
		*pixels_ok = 0;   /* the caller checks later (G13: after a flip that must have waited for it) */
		return 0;
	}
	rc = drmSyncobjWait(P.render, out_sync, 1, INT64_MAX, 0, NULL);
	if (rc != 0) {
		return rc;
	}
	for (i = 0; i < 64u * 64u; i++) {
		bad += (rt->cpu[i] != colour);
	}
	*pixels_ok = (bad == 0u);
	return 0;
}


static void t_render_clear(void)
{
	rbo_t rt, bcl, rcl, ta, ts;
	uint32_t bsz, rsz, tasz, tssz, s1 = 0, s2 = 0, s3 = 0, first = 99;
	struct drm_v3d_wait_bo wb;
	int rc, px1 = 0, px2 = 0, ok, sfd = -1, rc_exp = -1, rc_imp = -1, rc_w2 = -1, rc_tr = -1, rc_w3 = -1, rc_empty = 0,
		rc_sig = -1, rc_w4 = -1, wrc;
	uint64_t t0, t1;

	memset(&rt, 0, sizeof(rt));
	memset(&bcl, 0, sizeof(bcl));
	memset(&rcl, 0, sizeof(rcl));
	memset(&ta, 0, sizeof(ta));
	memset(&ts, 0, sizeof(ts));
	(void)v3da_clgen_clear_sizes(64u, 64u, &bsz, &rsz, &tasz, &tssz);
	rc = rbo_new(&rt, 64u * 64u * 4u);
	if (rc == 0) rc = rbo_new(&bcl, 4096u);
	if (rc == 0) rc = rbo_new(&rcl, 4096u);
	if (rc == 0) rc = rbo_new(&ta, tasz);
	if (rc == 0) rc = rbo_new(&ts, 4096u);
	if (rc == 0) rc = drmSyncobjCreate(P.render, 0, &s1);
	t0 = now_us();
	if (rc == 0) rc = cl_clear(&rt, &bcl, &rcl, &ta, &ts, 0xff3366ccu, 0u, &s1, &px1, 1);
	t1 = now_us();
	memset(&wb, 0, sizeof(wb));
	wb.handle = rt.handle;
	wb.timeout_ns = ~0ull;
	wrc = drmIoctl(P.render, DRM_IOCTL_V3D_WAIT_BO, &wb);
	ok = (rc == 0) && px1 && (wrc == 0);
	printf(TAG "cl_clear rc=%d pixels_ok=%d rt0=0x%08x wait_bo=%d us=%llu sizes=%u/%u/%u/%u ok=%d\n", rc, px1,
		(rt.cpu != NULL) ? rt.cpu[0] : 0u, wrc, (unsigned long long)(t1 - t0), bsz, rsz, tasz, tssz, ok);
	verdict("cl_clear", ok);

	/* a dependent second job (in_sync_bcl = the first job's syncobj) */
	if (rc == 0) {
		rc = cl_clear(&rt, &bcl, &rcl, &ta, &ts, 0xff00ff00u, s1, &s1, &px2, 1);
		printf(TAG "cl_clear_dep rc=%d pixels_ok=%d rt0=0x%08x ok=%d\n", rc, px2, rt.cpu[0], (rc == 0) && px2);
		verdict("cl_clear_dep", (rc == 0) && px2);
	}

	/* sync files (in-process emulation), transfer, reset/signal */
	if (rc == 0) {
		rc_exp = drmSyncobjExportSyncFile(P.render, s1, &sfd);
		if ((drmSyncobjCreate(P.render, 0, &s2) == 0) && (rc_exp == 0)) {
			rc_imp = drmSyncobjImportSyncFile(P.render, s2, sfd);
			rc_w2 = drmSyncobjWait(P.render, &s2, 1, INT64_MAX, 0, &first);
		}
		if (drmSyncobjCreate(P.render, 0, &s3) == 0) {
			rc_tr = drmSyncobjTransfer(P.render, s3, 0, s1, 0, 0);
			rc_w3 = drmSyncobjWait(P.render, &s3, 1, INT64_MAX, 0, NULL);
			(void)drmSyncobjReset(P.render, &s3, 1);
			rc_empty = drmSyncobjWait(P.render, &s3, 1, 0, 0, NULL);   /* empty, no WAIT_FOR_SUBMIT: -EINVAL */
			rc_sig = drmSyncobjSignal(P.render, &s3, 1);
			rc_w4 = drmSyncobjWait(P.render, &s3, 1, INT64_MAX, 0, NULL);
		}
		ok = (rc_exp == 0) && (sfd >= 0) && (rc_imp == 0) && (rc_w2 == 0) && (rc_tr == 0) && (rc_w3 == 0) &&
			(rc_empty == -EINVAL) && (rc_sig == 0) && (rc_w4 == 0);
		printf(TAG "syncobj export=%d sfd=%d import=%d wait=%d first=%u transfer=%d wait_t=%d wait_empty=%d signal=%d "
			"wait_s=%d ok=%d\n", rc_exp, sfd, rc_imp, rc_w2, first, rc_tr, rc_w3, rc_empty, rc_sig, rc_w4, ok);
		verdict("syncobj", ok);
	}

	/* M5 (G15 in-process): Linux sync_file ioctls on the emulated sync files, raw
	 * ioctl() as Mesa's libsync issues them (v3dv merge_syncobjs on every signalling
	 * vkQueueSubmit): merge a pending job fence with a signalled one, FILE_INFO,
	 * import the merge into a syncobj, wait. Needs -Wl,--wrap=ioctl. */
	if (rc == 0) {
		struct probe_sync_merge_data md;
		struct probe_sync_file_info fi;
		uint32_t s4 = 0, s5 = 0, s6 = 0;
		int fa = -1, fb = -1, rc_m = -1, e_m = 0, rc_i = -1, st = -1, rc_im = -1, rc_w = -1, px3 = 0, rc_j;
		unsigned nf = 99u;
		rc_j = drmSyncobjCreate(P.render, 0, &s4);
		if (rc_j == 0) rc_j = cl_clear(&rt, &bcl, &rcl, &ta, &ts, 0xff123456u, 0u, &s4, &px3, 0);   /* pending, not waited */
		if (rc_j == 0) rc_j = drmSyncobjCreate(P.render, DRM_SYNCOBJ_CREATE_SIGNALED, &s5);
		if (rc_j == 0) rc_j = drmSyncobjExportSyncFile(P.render, s4, &fa);
		if (rc_j == 0) rc_j = drmSyncobjExportSyncFile(P.render, s5, &fb);
		if (rc_j == 0) {
			memset(&md, 0, sizeof(md));
			strcpy(md.name, "drmprobe");
			md.fd2 = fb;
			rc_m = ioctl(fa, PROBE_SYNC_IOC_MERGE, &md);
			e_m = (rc_m != 0) ? errno : 0;
		}
		if (rc_m == 0) {
			memset(&fi, 0, sizeof(fi));
			rc_i = ioctl(md.fence, PROBE_SYNC_IOC_FILE_INFO, &fi);
			st = fi.status;
			nf = fi.num_fences;
			if (drmSyncobjCreate(P.render, 0, &s6) == 0) {
				rc_im = drmSyncobjImportSyncFile(P.render, s6, md.fence);
				rc_w = drmSyncobjWait(P.render, &s6, 1, INT64_MAX, 0, NULL);
			}
			close(md.fence);
		}
		ok = (rc_j == 0) && (rc_m == 0) && (md.fence >= 0) && (rc_i == 0) && (nf <= 1u) && (rc_im == 0) && (rc_w == 0);
		printf(TAG "sync_merge setup=%d merge=%d errno=%d mfd=%d info=%d status=%d nfences=%u import=%d wait=%d ok=%d gap=%d\n",
			rc_j, rc_m, e_m, (rc_m == 0) ? md.fence : -1, rc_i, st, nf, rc_im, rc_w, ok, e_m == ENOTTY);
		if ((rc_m != 0) && (e_m == ENOTTY)) {
			P.gap++;   /* no --wrap=ioctl / a library from before M5b: the sync_file ioctl reached the server */
		}
		else {
			verdict("sync_merge", ok);
		}
		if (fa >= 0) close(fa);
		if (fb >= 0) close(fb);
		if (s4 != 0u) (void)drmSyncobjDestroy(P.render, s4);
		if (s5 != 0u) (void)drmSyncobjDestroy(P.render, s5);
		if (s6 != 0u) (void)drmSyncobjDestroy(P.render, s6);
	}

	/* A sync file the program duplicates itself is still that sync file. Mesa's EGL
	 * native-fence path, exactly: eglCreateSyncKHR(fd) -> create_fence_fd
	 * fcntl(F_DUPFD_CLOEXEC, 3), the caller closes its fd, eglWaitSyncKHR ->
	 * sync_accumulate os_dupfd_cloexec (F_DUPFD_CLOEXEC again), and the next submit
	 * imports that copy into its in-syncobj (v3d_job.c "Failed to import native
	 * fence." when it cannot). Plus dup() and dup2() of it, and a stale entry: the
	 * exported descriptor's number, closed and reused by dup2() of the render node,
	 * must no longer be a sync file. Needs -Wl,--wrap=fcntl,--wrap=dup,--wrap=dup2. */
	if (rc == 0) {
		struct probe_sync_file_info fi;
		uint32_t s7 = 0, s8 = 0, s9 = 0;
		int fa = -1, d1 = -1, d2 = -1, d3 = -1, d4 = -1, r = -1, rc_j, rc_im = -1, e_im = 0, rc_w = -1, rc_im2 = -1,
			rc_w2 = -1, rc_st = -2, e_st = 0, px4 = 0, num = -1;
		rc_j = drmSyncobjCreate(P.render, 0, &s7);
		if (rc_j == 0) rc_j = cl_clear(&rt, &bcl, &rcl, &ta, &ts, 0xff654321u, 0u, &s7, &px4, 0);   /* pending, not waited */
		if (rc_j == 0) rc_j = drmSyncobjExportSyncFile(P.render, s7, &fa);
		if (rc_j == 0) {
			/* The graded copies land on numbers no earlier test used (Mesa passes a
			 * minimum of 3): a number an earlier sync file had keeps its stale table
			 * entry in a library without the interposers, and an import of it then
			 * "succeeds" with that old fence - which would hide the failure. */
			d1 = fcntl(fa, F_DUPFD_CLOEXEC, 150);   /* v3d_fence_create_fd */
			d2 = (d1 >= 0) ? fcntl(d1, F_DUPFD_CLOEXEC, 150) : -1;   /* sync_accumulate -> os_dupfd_cloexec */
			d3 = (d2 >= 0) ? dup(d2) : -1;
			d4 = (d3 >= 0) ? dup2(d3, 160) : -1;
			num = fa;
			close(fa);   /* the application closes its own descriptor (EGL owns the copy) */
			fa = -1;
		}
		if ((d2 >= 0) && (drmSyncobjCreate(P.render, 0, &s8) == 0)) {
			rc_im = drmSyncobjImportSyncFile(P.render, s8, d2);
			e_im = (rc_im != 0) ? errno : 0;
			rc_w = drmSyncobjWait(P.render, &s8, 1, INT64_MAX, 0, NULL);
		}
		if ((d4 >= 0) && (drmSyncobjCreate(P.render, 0, &s9) == 0)) {
			rc_im2 = drmSyncobjImportSyncFile(P.render, s9, d4);   /* the job is done by now: a signalled snapshot */
			rc_w2 = drmSyncobjWait(P.render, &s9, 1, INT64_MAX, 0, NULL);
		}
		if (num >= 0) {
			r = dup2(P.render, num);   /* the closed export's number, now a plain render descriptor */
			if (r == num) {
				memset(&fi, 0, sizeof(fi));
				rc_st = ioctl(r, PROBE_SYNC_IOC_FILE_INFO, &fi);
				e_st = (rc_st != 0) ? errno : 0;
			}
		}
		ok = (rc_j == 0) && (d1 >= 0) && (d2 >= 0) && (d3 >= 0) && (d4 == 160) && (rc_im == 0) && (rc_w == 0) &&
			(rc_im2 == 0) && (rc_w2 == 0) && (r == num) && (rc_st == -1) && (e_st == ENOTTY);
		printf(TAG "sync_dup setup=%d dups=%d,%d,%d,%d import=%d errno=%d wait=%d import_dup2=%d wait_dup2=%d "
			"stale_info=%d stale_errno=%d ok=%d\n", rc_j, d1, d2, d3, d4, rc_im, e_im, rc_w, rc_im2, rc_w2, rc_st, e_st, ok);
		verdict("sync_dup", ok);
		if (r >= 0) close(r);
		if (d1 >= 0) close(d1);
		if (d2 >= 0) close(d2);
		if (d3 >= 0) close(d3);
		if (d4 >= 0) close(d4);
		if (s7 != 0u) (void)drmSyncobjDestroy(P.render, s7);
		if (s8 != 0u) (void)drmSyncobjDestroy(P.render, s8);
		if (s9 != 0u) (void)drmSyncobjDestroy(P.render, s9);
	}

	/* cross-server fence: flip to a buffer only after a GPU job, IN_FENCE_FD = the sync file */
	if ((rc == 0) && (sfd >= 0) && (P.primary != 0u) && (P.buf[0].fb != 0u)) {
		drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(P.card, P.primary, DRM_MODE_OBJECT_PLANE);
		drmModeAtomicReqPtr req = drmModeAtomicAlloc();
		uint32_t fb_prop = 0, fence_prop = 0;
		char nb[40];
		int j, rcf;
		for (j = 0; (props != NULL) && (j < (int)props->count_props); j++) {
			prop_name_of(P.card, props->props[j], nb, sizeof(nb));
			if (strcmp(nb, "FB_ID") == 0) fb_prop = props->props[j];
			if (strcmp(nb, "IN_FENCE_FD") == 0) fence_prop = props->props[j];
		}
		drmModeFreeObjectProperties(props);
		(void)drmModeAtomicAddProperty(req, P.primary, fb_prop, P.buf[0].fb);
		(void)drmModeAtomicAddProperty(req, P.primary, fence_prop, (uint64_t)sfd);
		P.flips_done = 0;
		rcf = drmModeAtomicCommit(P.card, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL);
		if (rcf == 0) {
			rcf = wait_flips(1, 3000);
		}
		drmModeAtomicFree(req);
		printf(TAG "in_fence fence_prop=0x%x commit=%d errno=%d events=%d ok=%d\n", fence_prop, rcf,
			(rcf != 0) ? errno : 0, P.flips_done, rcf == 0);
		verdict("in_fence", rcf == 0);
	}
	if (sfd >= 0) {
		close(sfd);
	}
	if (s1 != 0u) (void)drmSyncobjDestroy(P.render, s1);
	if (s2 != 0u) (void)drmSyncobjDestroy(P.render, s2);
	if (s3 != 0u) (void)drmSyncobjDestroy(P.render, s3);
	rbo_free(&rt);
	rbo_free(&bcl);
	rbo_free(&rcl);
	rbo_free(&ta);
	rbo_free(&ts);
}


/* ========================================================================= */
/* 5. PRIME                                                                   */
/* ========================================================================= */

/* G1 round trip (M3 part 2): a kms dumb buffer, PRIME-exported and imported on
 * the render node, is the render target of a GPU clear; the CPU reads the result
 * back through the kms mapping (the same pages, three mappings: kms server, v3d
 * server, this process). Then G13: a second clear is submitted and NOT waited
 * for, and the buffer is flipped with no IN_FENCE_FD - libdrm-phoenix attaches
 * the BO's last-use fence, rpi4-kms -G holds the flip until the GPU is done, so
 * the pixels are complete when the flip event arrives. */
static void t_import_rt(uint32_t h_render)
{
	rbo_t rt, bcl, rcl, ta, ts;
	struct drm_v3d_get_bo_offset go;
	struct drm_v3d_wait_bo wb;
	uint32_t bsz, rsz, tasz, tssz, s = 0, i, bad = 0;
	int rc, px = 0, wrc = -1, rcs = -1, rcf = -1, ok;
	uint64_t t0 = 0, t1 = 0;

	memset(&rt, 0, sizeof(rt));
	memset(&bcl, 0, sizeof(bcl));
	memset(&rcl, 0, sizeof(rcl));
	memset(&ta, 0, sizeof(ta));
	memset(&ts, 0, sizeof(ts));
	memset(&go, 0, sizeof(go));
	(void)v3da_clgen_clear_sizes(64u, 64u, &bsz, &rsz, &tasz, &tssz);
	go.handle = h_render;
	rc = (drmIoctl(P.render, DRM_IOCTL_V3D_GET_BO_OFFSET, &go) == 0) ? 0 : -errno;
	rt.handle = h_render;
	rt.offset = go.offset;
	rt.size = 64u * 64u * 4u;
	rt.cpu = P.buf[0].px;   /* the kms dumb mapping: the CPU view of the imported pages */
	if (rc == 0) rc = rbo_new(&bcl, 4096u);
	if (rc == 0) rc = rbo_new(&rcl, 4096u);
	if (rc == 0) rc = rbo_new(&ta, tasz);
	if (rc == 0) rc = rbo_new(&ts, 4096u);
	if (rc == 0) rc = drmSyncobjCreate(P.render, 0, &s);
	if (rc == 0) rc = cl_clear(&rt, &bcl, &rcl, &ta, &ts, 0xff2080ffu, 0u, &s, &px, 1);
	memset(&wb, 0, sizeof(wb));
	wb.handle = h_render;
	wb.timeout_ns = ~0ull;
	if (rc == 0) {
		wrc = drmIoctl(P.render, DRM_IOCTL_V3D_WAIT_BO, &wb);   /* an imported BO: the server's BO_WAIT */
	}
	ok = (rc == 0) && px && (wrc == 0);
	printf(TAG "import_clear handle=0x%x gpuva=0x%x rc=%d pixels_ok=%d px0=0x%08x wait_bo=%d ok=%d\n", h_render, go.offset, rc,
		px, (P.buf[0].px != NULL) ? P.buf[0].px[0] : 0u, wrc, ok);
	verdict("import_clear", ok);

	/* G13: show buf[1], render buf[0] without waiting, flip to buf[0] with no fence */
	if ((rc == 0) && (P.buf[1].fb != 0u)) {
		P.flips_done = 0;
		if ((drmModePageFlip(P.card, P.crtc, P.buf[1].fb, DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0) &&
				(wait_flips(1, 3000) == 0)) {
			rcs = cl_clear(&rt, &bcl, &rcl, &ta, &ts, 0xff80ff20u, 0u, &s, &px, 0);
			t0 = now_us();
			P.flips_done = 0;
			rcf = (rcs == 0) ? drmModePageFlip(P.card, P.crtc, P.buf[0].fb, DRM_MODE_PAGE_FLIP_EVENT, NULL) : -1;
			if (rcf == 0) {
				rcf = wait_flips(1, 3000);
			}
			t1 = now_us();
			for (i = 0; i < 64u * 64u; i++) {
				bad += (P.buf[0].px[i] != 0xff80ff20u);   /* read BEFORE any explicit wait */
			}
		}
		ok = (rcs == 0) && (rcf == 0) && (P.flips_done == 1) && (bad == 0u);
		printf(TAG "implicit_flip submit=%d flip=%d events=%d pixels_ok=%d px0=0x%08x flip_us=%llu ok=%d\n", rcs, rcf,
			P.flips_done, bad == 0u, P.buf[0].px[0], (unsigned long long)(t1 - t0), ok);
		verdict("implicit_flip", ok);
		(void)drmSyncobjWait(P.render, &s, 1, INT64_MAX, 0, NULL);   /* nothing in flight before the BOs go */
	}
	if (s != 0u) {
		(void)drmSyncobjDestroy(P.render, s);
	}
	rbo_free(&bcl);
	rbo_free(&rcl);
	rbo_free(&ta);
	rbo_free(&ts);
}

static void t_prime(void)
{
	char path[64] = "-";
	uint32_t h_self = 0, h_render = 0;
	int pfd = -1, rc_exp, rc_self = -1, rc_imp = -1, e_imp = 0, same = 0;
	uint32_t *m;
	struct drm_gem_close gc;

	rc_exp = drmPrimeHandleToFD(P.card, P.buf[0].handle, DRM_CLOEXEC, &pfd);
	if (rc_exp == 0) {
		/* G3: a dma-buf's size by lseek(SEEK_END), as Mesa reads it (v3d_bufmgr.c:454) */
		off_t end = lseek(pfd, 0, SEEK_END);
		int e_end = (end < 0) ? errno : 0, ok_end = (end == (off_t)P.buf[0].size);
		(void)lseek(pfd, 0, SEEK_SET);
		printf(TAG "dmabuf_size end=%lld errno=%d want=%llu ok=%d gap=%d\n", (long long)end, e_end,
			(unsigned long long)P.buf[0].size, ok_end, (end < 0) && (e_end == ENOENT));
		if ((end < 0) && (e_end == ENOENT)) {
			P.gap++;   /* rpi4-kms from before M3 part 2 refuses atSize */
		}
		else {
			verdict("dmabuf_size", ok_end);
		}
		(void)sys_fdpath(pfd, path, sizeof(path));
		m = mmap(NULL, (size_t)P.buf[0].size, PROT_READ, MAP_SHARED, pfd, 0);
		if (m != MAP_FAILED) {
			same = (m[0] == P.buf[0].px[0]) && (m[P.buf[0].size / 8u] == P.buf[0].px[P.buf[0].size / 8u]);
			P.buf[0].px[1] ^= 0x00ffffffu;   /* write through the dumb mapping, read through the dma-buf */
			same = same && (m[1] == P.buf[0].px[1]);
			(void)munmap(m, (size_t)P.buf[0].size);
		}
		rc_self = drmPrimeFDToHandle(P.card, pfd, &h_self);
		rc_imp = drmPrimeFDToHandle(P.render, pfd, &h_render);
		e_imp = (rc_imp != 0) ? errno : 0;
	}
	printf(TAG "prime_export rc=%d fd=%d path=%s mmap_same_pages=%d self_import=%d handle=%u/%u ok=%d\n", rc_exp, pfd,
		path, same, rc_self, h_self, P.buf[0].handle, (rc_exp == 0) && same && (rc_self == 0) && (h_self == P.buf[0].handle));
	verdict("prime_export", (rc_exp == 0) && same && (rc_self == 0) && (h_self == P.buf[0].handle));
	printf(TAG "prime_import_render rc=%d errno=%d handle=0x%x ok=%d gap=%d\n", rc_imp, e_imp, h_render,
		(rc_imp == 0) && (h_render != 0u), e_imp == ENOSYS);
	gapcheck("prime_import_render", e_imp, ENOSYS);   /* ENOSYS: a render server from before M3 part 2 (G1) */
	if (pfd >= 0) {
		close(pfd);   /* the import holds the pages through the render server's own mapping (E1) */
	}
	if ((rc_imp == 0) && (h_render != 0u)) {
		uint32_t h_again = 0;
		int rc_again = -1;
		pfd = -1;
		if (drmPrimeHandleToFD(P.card, P.buf[0].handle, DRM_CLOEXEC, &pfd) == 0) {
			rc_again = drmPrimeFDToHandle(P.render, pfd, &h_again);   /* DRM: same buffer, same handle */
			close(pfd);
		}
		printf(TAG "prime_reimport rc=%d handle=0x%x same=%d ok=%d\n", rc_again, h_again, h_again == h_render,
			(rc_again == 0) && (h_again == h_render));
		verdict("prime_reimport", (rc_again == 0) && (h_again == h_render));
		{
			/* M5 (G4a): the v3dv WSI round trip -- vkGetMemoryFdKHR exports the imported
			 * BO from the render node, wsi_common_display imports that descriptor on card0
			 * and must get the original dumb handle back, same pages. */
			char rpath[64] = "-";
			uint32_t h_back = 0, *rm;
			int rfd = -1, rc_rx, e_rx, rc_back = -1, same_rx = 0;
			rc_rx = drmPrimeHandleToFD(P.render, h_render, DRM_CLOEXEC | DRM_RDWR, &rfd);
			e_rx = (rc_rx != 0) ? errno : 0;
			if (rc_rx == 0) {
				(void)sys_fdpath(rfd, rpath, sizeof(rpath));
				rc_back = drmPrimeFDToHandle(P.card, rfd, &h_back);
				rm = mmap(NULL, (size_t)P.buf[0].size, PROT_READ, MAP_SHARED, rfd, 0);
				if (rm != MAP_FAILED) {
					same_rx = (rm[1] == P.buf[0].px[1]) && (rm[P.buf[0].size / 8u] == P.buf[0].px[P.buf[0].size / 8u]);
					(void)munmap(rm, (size_t)P.buf[0].size);
				}
				close(rfd);
			}
			printf(TAG "prime_reexport_render rc=%d errno=%d path=%s card_handle=%u/%u same_pages=%d ok=%d gap=%d\n",
				rc_rx, e_rx, rpath, h_back, P.buf[0].handle, same_rx,
				(rc_rx == 0) && (rc_back == 0) && (h_back == P.buf[0].handle) && same_rx, e_rx == ENOSYS);
			if (e_rx == ENOSYS) {
				P.gap++;   /* a libdrm-phoenix from before M5 */
			}
			else {
				verdict("prime_reexport_render", (rc_rx == 0) && (rc_back == 0) && (h_back == P.buf[0].handle) && same_rx);
			}
		}
		t_import_rt(h_render);
		memset(&gc, 0, sizeof(gc));
		gc.handle = h_render;
		(void)drmIoctl(P.render, DRM_IOCTL_GEM_CLOSE, &gc);   /* the one close releases it (the server logs "import released") */
	}

}


/* ========================================================================= */
/* 6. render-node PRIME export (G4: V3DA_OP_BO_EXPORT + /v3dbuf)              */
/* ========================================================================= */

#define XP_SIZE   65536u
#define XP_WORDS  (XP_SIZE / 4u)
#define XP_MARKER 0x4734a11du

static uint32_t xp_word(uint32_t i)
{
	return 0x6b000000u ^ (i * 2654435761u);
}


static uint32_t xp_bad(const volatile uint32_t *p, uint32_t skip_a, uint32_t skip_b)
{
	uint32_t i, bad = 0;

	for (i = 0; i < XP_WORDS; i++) {
		if ((i != skip_a) && (i != skip_b) && (p[i] != xp_word(i))) {
			bad++;
		}
	}
	return bad;
}


/* A handle's CPU view through a given render connection: MMAP_BO token + mmap. */
static uint32_t *xp_map_handle(int fd, uint32_t handle)
{
	struct drm_v3d_mmap_bo mb;
	void *p;

	memset(&mb, 0, sizeof(mb));
	mb.handle = handle;
	if (drmIoctl(fd, DRM_IOCTL_V3D_MMAP_BO, &mb) != 0) {
		return NULL;
	}
	p = mmap(NULL, XP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)mb.offset);
	return (p == MAP_FAILED) ? NULL : p;
}


/* The server still knows `handle` on `fd`: WAIT_BO of a handle this connection
 * imported asks the server (a released BO answers EINVAL). */
static int xp_alive(int fd, uint32_t handle)
{
	struct drm_v3d_wait_bo wb;

	memset(&wb, 0, sizeof(wb));
	wb.handle = handle;
	return (drmIoctl(fd, DRM_IOCTL_V3D_WAIT_BO, &wb) == 0) ? 1 : 0;
}


static void gem_close(int fd, uint32_t handle)
{
	struct drm_gem_close gc;

	memset(&gc, 0, sizeof(gc));
	gc.handle = handle;
	(void)drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
}


/* Same process: export a render BO, check the descriptor is a /v3dbuf name that
 * sizes, fstat()s and mmap()s onto the BO's pages (both directions), that a second
 * export names the same buffer and that importing it back returns THE handle
 * (DRM). Then a second render connection imports it: it shares the pages, keeps
 * the BO alive after the creator's GEM_CLOSE, and one GEM_CLOSE releases it. */
static void t_prime_render(void)
{
	char path[64] = "-", path2[64] = "-";
	struct stat st;
	rbo_t b;
	uint32_t h_self = 0, h2 = 0, *m = NULL, *m2 = NULL, bad = 0, bad2 = 0;
	off_t end = -1;
	int rc, e = 0, fd = -1, fd2 = -1, rc_self = -1, chr = 0, same_name = 0, xw = 0, conn2 = -1, rc2 = -1, e2 = 0;
	int survives = 0, released = 0, ok;

	rc = rbo_new(&b, XP_SIZE);
	if (rc == 0) {
		uint32_t i;
		for (i = 0; i < XP_WORDS; i++) {
			b.cpu[i] = xp_word(i);
		}
		rc = drmPrimeHandleToFD(P.render, b.handle, DRM_CLOEXEC | DRM_RDWR, &fd);
		e = (rc != 0) ? errno : 0;
	}
	if ((rc == 0) && (fd >= 0)) {
		(void)sys_fdpath(fd, path, sizeof(path));
		end = lseek(fd, 0, SEEK_END);
		(void)lseek(fd, 0, SEEK_SET);
		chr = (fstat(fd, &st) == 0) && S_ISCHR(st.st_mode);
		m = mmap(NULL, XP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (m == MAP_FAILED) {
			m = NULL;
		}
		if (m != NULL) {
			bad = xp_bad(m, ~0u, ~0u);
			m[7] = XP_MARKER;          /* dma-buf mapping -> BO mapping */
			b.cpu[9] = ~XP_MARKER;     /* BO mapping -> dma-buf mapping */
			xw = (b.cpu[7] == XP_MARKER) && (m[9] == ~XP_MARKER);
			b.cpu[7] = xp_word(7);
			b.cpu[9] = xp_word(9);
		}
		if (drmPrimeHandleToFD(P.render, b.handle, DRM_CLOEXEC, &fd2) == 0) {
			(void)sys_fdpath(fd2, path2, sizeof(path2));
			same_name = (strcmp(path, path2) == 0);
			close(fd2);
		}
		rc_self = drmPrimeFDToHandle(P.render, fd, &h_self);
	}
	ok = (rc == 0) && (strncmp(path, "/v3dbuf/", 8) == 0) && (end == (off_t)XP_SIZE) && chr && (m != NULL) &&
		(bad == 0u) && xw && same_name && (rc_self == 0) && (h_self == b.handle);
	printf(TAG "prime_export_render rc=%d errno=%d path=%s size=%lld fstat_chr=%d mmap=%d bad_words=%u xwrite=%d "
		"reexport_same_name=%d self_import=%d handle=0x%x/0x%x ok=%d\n", rc, e, path, (long long)end, chr, m != NULL, bad,
		xw, same_name, rc_self, h_self, b.handle, ok);
	verdict("prime_export_render", ok);

	/* a second render connection of this process = another server client */
	if (ok) {
		conn2 = open(P.render_path, O_RDWR | O_CLOEXEC);
		if (conn2 >= 0) {
			rc2 = drmPrimeFDToHandle(conn2, fd, &h2);
			e2 = (rc2 != 0) ? errno : 0;
		}
		if (rc2 == 0) {
			m2 = xp_map_handle(conn2, h2);
			bad2 = (m2 != NULL) ? xp_bad(m2, ~0u, ~0u) : XP_WORDS;
			close(fd);   /* the descriptor's reference goes ... */
			fd = -1;
			gem_close(P.render, b.handle);   /* ... and the creator's: conn2's import alone holds it */
			(void)munmap(b.cpu, b.size);
			b.cpu = NULL;
			b.handle = 0;
			survives = xp_alive(conn2, h2) && (m2 != NULL) && (m2[1234] == xp_word(1234));
			if (m2 != NULL) {
				(void)munmap(m2, XP_SIZE);
			}
			gem_close(conn2, h2);            /* one close releases the import */
			released = !xp_alive(conn2, h2);
		}
		ok = (rc2 == 0) && (m2 != NULL) && (bad2 == 0u) && survives && released;
		printf(TAG "prime_import_render2 conn=%d rc=%d errno=%d handle=0x%x map=%d bad_words=%u survives_creator_close=%d "
			"released=%d ok=%d\n", conn2 >= 0, rc2, e2, h2, m2 != NULL, bad2, survives, released, ok);
		verdict("prime_import_render2", ok);
		if (conn2 >= 0) {
			close(conn2);
		}
	}
	else {
		printf(TAG "prime_import_render2 skipped=1 (prime_export_render failed) ok=0\n");
		verdict("prime_import_render2", 0);
	}
	if (m != NULL) {
		(void)munmap(m, XP_SIZE);
	}
	if (fd >= 0) {
		close(fd);
	}
	rbo_free(&b);
}


#ifndef DRMPROBE_NO_FORK
/* libphoenix tags sendmsg()/recvmsg() "not fully supported"; the single-iovec
 * SCM_RIGHTS use here goes straight to the kernel's fdpass code (exportprobe). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattribute-warning"

typedef struct {
	uint32_t cmd;           /* 1 = descriptor attached, 2 = child report (G6: 11..15, see t_g6_xproc) */
	int32_t rc, err;
	uint32_t handle;
	int32_t size_ok, path_ok, fd_map, fd_bad, handle_map, handle_bad, xwrite, wait_bo;
	uint64_t aux[4];        /* G6: the producer's timings */
} xp_msg_t;


static int xp_send(int s, const xp_msg_t *c, int fd)
{
	union {
		struct cmsghdr h;
		char buf[CMSG_SPACE(sizeof(int))];
	} cm;
	struct msghdr mh;
	struct iovec iov;
	struct cmsghdr *h;

	memset(&mh, 0, sizeof(mh));
	iov.iov_base = (void *)c;
	iov.iov_len = sizeof(*c);
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	if (fd >= 0) {
		memset(&cm, 0, sizeof(cm));
		mh.msg_control = cm.buf;
		mh.msg_controllen = sizeof(cm.buf);
		h = CMSG_FIRSTHDR(&mh);
		h->cmsg_level = SOL_SOCKET;
		h->cmsg_type = SCM_RIGHTS;
		h->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(h), &fd, sizeof(int));
	}
	return (sendmsg(s, &mh, 0) == (ssize_t)sizeof(*c)) ? 0 : -1;
}


static int xp_recv(int s, xp_msg_t *c, uint32_t want, int *fd)
{
	union {
		struct cmsghdr h;
		char buf[CMSG_SPACE(sizeof(int))];
	} cm;
	struct msghdr mh;
	struct iovec iov;
	struct cmsghdr *h;
	ssize_t n;

	memset(&mh, 0, sizeof(mh));
	memset(&cm, 0, sizeof(cm));
	iov.iov_base = c;
	iov.iov_len = sizeof(*c);
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	mh.msg_control = cm.buf;
	mh.msg_controllen = sizeof(cm.buf);
	do {
		n = recvmsg(s, &mh, 0);
	} while ((n < 0) && (errno == EINTR));
	if ((n != (ssize_t)sizeof(*c)) || (c->cmd != want)) {
		return -1;
	}
	if (fd != NULL) {
		*fd = -1;
		h = CMSG_FIRSTHDR(&mh);
		if ((h != NULL) && (h->cmsg_level == SOL_SOCKET) && (h->cmsg_type == SCM_RIGHTS)) {
			memcpy(fd, CMSG_DATA(h), sizeof(int));
		}
	}
	return 0;
}

#pragma GCC diagnostic pop


/* The importer process: its OWN render connection (the inherited descriptors share
 * the parent's open file, i.e. the parent's server client), the dma-buf descriptor
 * from SCM_RIGHTS. By the time it imports, the exporter has closed its handle and its
 * descriptor: only the descriptor in flight keeps the BO. */
static void xp_child(int s)
{
	char path[64] = "-";
	xp_msg_t r;
	uint32_t *fm = NULL, *hm = NULL;
	int fd = -1, rfd;

	memset(&r, 0, sizeof(r));
	r.cmd = 2;
	r.rc = -1;
	if (xp_recv(s, &r, 1, &fd) != 0) {
		r.err = EPROTO;
	}
	r.cmd = 2;
	rfd = open(P.render_path, O_RDWR | O_CLOEXEC);
	if ((fd >= 0) && (rfd >= 0)) {
		r.path_ok = (sys_fdpath(fd, path, sizeof(path)) > 0) && (strncmp(path, "/v3dbuf/", 8) == 0);
		r.size_ok = (lseek(fd, 0, SEEK_END) == (off_t)XP_SIZE);
		r.rc = drmPrimeFDToHandle(rfd, fd, &r.handle);
		r.err = (r.rc != 0) ? errno : 0;
		fm = mmap(NULL, XP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		fm = (fm == MAP_FAILED) ? NULL : fm;
		r.fd_map = (fm != NULL);
		r.fd_bad = (fm != NULL) ? (int32_t)xp_bad(fm, ~0u, ~0u) : -1;
		if (r.rc == 0) {
			hm = xp_map_handle(rfd, r.handle);
			r.handle_map = (hm != NULL);
			r.handle_bad = (hm != NULL) ? (int32_t)xp_bad(hm, ~0u, ~0u) : -1;
			if ((fm != NULL) && (hm != NULL)) {
				fm[3] = XP_MARKER;
				r.xwrite = (hm[3] == XP_MARKER);
			}
			r.wait_bo = xp_alive(rfd, r.handle);
		}
		if (hm != NULL) {
			(void)munmap(hm, XP_SIZE);
		}
		if (fm != NULL) {
			(void)munmap(fm, XP_SIZE);
		}
		if (r.rc == 0) {
			gem_close(rfd, r.handle);
		}
	}
	if (fd >= 0) {
		close(fd);
	}
	if (rfd >= 0) {
		close(rfd);
	}
	printf(TAG "prime_export_xproc child pid=%d path=%s import=%d errno=%d handle=0x%x\n", (int)getpid(), path, r.rc,
		r.err, r.handle);
	fflush(stdout);
	(void)xp_send(s, &r, -1);
}


/* Two processes, as a Wayland client and compositor: fork, export in the parent,
 * pass the descriptor over an AF_UNIX socketpair (SCM_RIGHTS), drop every
 * reference of the parent's, import + map + write in the child; afterwards the
 * BO must be gone (no leak). */
static void t_prime_xproc(void)
{
	xp_msg_t q, r;
	rbo_t b;
	uint32_t h = 0;
	pid_t pid = -1;
	int sv[2] = { -1, -1 }, rc, e = 0, fd = -1, got = -1, status = 0, released = 0, ok;

	memset(&r, 0, sizeof(r));
	rc = rbo_new(&b, XP_SIZE);
	if (rc == 0) {
		uint32_t i;
		for (i = 0; i < XP_WORDS; i++) {
			b.cpu[i] = xp_word(i);
		}
		rc = drmPrimeHandleToFD(P.render, b.handle, DRM_CLOEXEC, &fd);
		e = (rc != 0) ? errno : 0;
	}
	if ((rc == 0) && (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)) {
		rc = -1;
		e = errno;
	}
	if (rc == 0) {
		fflush(stdout);
		pid = fork();
		if (pid == 0) {
			close(sv[0]);
			xp_child(sv[1]);
			_exit(0);   /* no atexit/stdio teardown of the parent's libdrm state */
		}
		close(sv[1]);
		sv[1] = -1;
	}
	if (pid > 0) {
		memset(&q, 0, sizeof(q));
		q.cmd = 1;
		(void)xp_send(sv[0], &q, fd);
		h = b.handle;
		close(fd);   /* the exporter lets go of everything: descriptor, handle, mapping */
		fd = -1;
		rbo_free(&b);
		got = xp_recv(sv[0], &r, 2, NULL);
		(void)waitpid(pid, &status, 0);
		released = !xp_alive(P.render, h);   /* a handle this connection closed: the server answers */
	}
	else {
		rbo_free(&b);
	}
	ok = (pid > 0) && (got == 0) && (r.rc == 0) && r.path_ok && r.size_ok && r.fd_map && (r.fd_bad == 0) &&
		r.handle_map && (r.handle_bad == 0) && r.xwrite && r.wait_bo && released;
	printf(TAG "prime_export_xproc export=%d errno=%d fork=%d report=%d import=%d import_errno=%d path_ok=%d size_ok=%d "
		"fd_map=%d fd_bad=%d handle_map=%d handle_bad=%d xwrite=%d wait_bo=%d released=%d ok=%d\n", rc, e, pid > 0, got == 0,
		r.rc, r.err, r.path_ok, r.size_ok, r.fd_map, r.fd_bad, r.handle_map, r.handle_bad, r.xwrite, r.wait_bo, released, ok);
	verdict("prime_export_xproc", ok);
	if (fd >= 0) {
		close(fd);
	}
	if (sv[0] >= 0) {
		close(sv[0]);
	}
	if (sv[1] >= 0) {
		close(sv[1]);
	}
}
#endif


/* ========================================================================= */
/* G7: a render-node BO scanned out on card0                                  */
/* ========================================================================= */

static int name_open_errno(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0) {
		return errno;
	}
	close(fd);
	return 0;
}


/* A render BO (linear 32 bpp, the mode's size, pitch 64-aligned) exported as
 * /v3dbuf/<id>, imported on card0 (PRIME_FD_TO_HANDLE of a foreign buffer: G7),
 * refused as UIF and with a short pitch, added as a LINEAR XRGB8888 framebuffer and
 * flipped onto the primary plane. Then EVERY client reference goes while it is on
 * screen (dma-buf fd, render handle, CPU mapping, card0 handle): the buffer's name
 * must stay alive - rpi4-kms still holds it for the plane - until the plane has
 * moved off it and the framebuffer is removed; then the name is gone. If the
 * server finds the buffer above 1 GiB (the render server's blocks have no
 * placement guarantee), ADDFB2 answers EINVAL (`KMS fb FAIL ... why=above_1g`):
 * graded gap=1, not a failure. */
static void t_prime_card0(void)
{
	char path[64] = "-";
	uint32_t w = (P.mode.hdisplay != 0u) ? P.mode.hdisplay : 1920u, h = (P.mode.vdisplay != 0u) ? P.mode.vdisplay : 1080u;
	uint32_t pitch = ((w * 4u) + 63u) & ~63u, kh = 0, kh2 = 0, fb = 0, fb_uif = 0, fb_pitch = 0, x, y;
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	uint64_t mods[4] = { 0 };
	rbo_t b;
	int rc, rc_exp = -1, e = 0, fd = -1, rc_imp = -1, e_imp = 0, rc_imp2 = -1, rc_uif = 0, e_uif = 0, rc_pitch = 0, e_pitch = 0;
	int rc_fb = -1, e_fb = 0, shown = 0, alive = -1, back = 0, rc_rm = -1, gone = -1, refused = 0, ok;

	rc = rbo_new(&b, pitch * h);
	if (rc == 0) {
		/* 8 vertical colour bands, rows written once (uncached: ~10-60 ms) */
		static const uint32_t band[8][3] = { { 255, 0, 0 }, { 255, 128, 0 }, { 255, 255, 0 }, { 0, 255, 0 },
			{ 0, 255, 255 }, { 0, 0, 255 }, { 255, 0, 255 }, { 255, 255, 255 } };
		for (y = 0; y < h; y++) {
			for (x = 0; x < w; x++) {
				const uint32_t *c = band[(x * 8u) / w];
				b.cpu[y * (pitch / 4u) + x] = rgb(c[0], c[1], c[2]);
			}
		}
		rc_exp = drmPrimeHandleToFD(P.render, b.handle, DRM_CLOEXEC | DRM_RDWR, &fd);
		e = (rc_exp != 0) ? errno : 0;
	}
	else {
		e = -rc;
	}
	if ((rc_exp == 0) && (fd >= 0)) {
		(void)sys_fdpath(fd, path, sizeof(path));
		rc_imp = drmPrimeFDToHandle(P.card, fd, &kh);
		e_imp = (rc_imp != 0) ? errno : 0;
	}
	if (rc_imp == 0) {
		rc_imp2 = drmPrimeFDToHandle(P.card, fd, &kh2);   /* DRM: the same buffer again = the same handle */
		handles[0] = kh;
		pitches[0] = pitch;
		mods[0] = DRM_FORMAT_MOD_BROADCOM_UIF;
		rc_uif = drmModeAddFB2WithModifiers(P.card, w, h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, mods, &fb_uif,
			DRM_MODE_FB_MODIFIERS);
		e_uif = (rc_uif != 0) ? errno : 0;
		mods[0] = DRM_FORMAT_MOD_LINEAR;
		pitches[0] = pitch / 2u;
		rc_pitch = drmModeAddFB2WithModifiers(P.card, w, h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, mods,
			&fb_pitch, DRM_MODE_FB_MODIFIERS);
		e_pitch = (rc_pitch != 0) ? errno : 0;
		pitches[0] = pitch;
		rc_fb = drmModeAddFB2WithModifiers(P.card, w, h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, mods, &fb,
			DRM_MODE_FB_MODIFIERS);
		e_fb = (rc_fb != 0) ? errno : 0;
		refused = (rc_fb != 0) && (e_fb == EINVAL);
	}
	if ((rc_fb == 0) && (P.crtc != 0u) && (P.buf[0].fb != 0u)) {
		P.flips_done = 0;
		rc = drmModePageFlip(P.card, P.crtc, fb, DRM_MODE_PAGE_FLIP_EVENT, NULL);
		shown = (rc == 0) && (wait_flips(1, 3000) == 0);
		printf(TAG "prime_import_card0 shown=%d fb=%u handle=%u path=%s (HDMI: 8 vertical colour bands)\n", shown, fb, kh,
			path);
		if (shown) {
			usleep(3000000);   /* long enough for a dense HDMI snapshot */
		}
		/* every client reference goes while the plane shows the buffer */
		close(fd);
		fd = -1;
		rbo_free(&b);
		gem_close(P.card, kh);
		kh = 0;
		alive = name_open_errno(path);   /* 0: the name still resolves (rpi4-kms holds it) */
		if (shown) {
			P.flips_done = 0;
			rc = drmModePageFlip(P.card, P.crtc, P.buf[0].fb, DRM_MODE_PAGE_FLIP_EVENT, NULL);
			back = (rc == 0) && (wait_flips(1, 3000) == 0);
		}
		rc_rm = drmModeRmFB(P.card, fb);
		fb = 0;
		gone = name_open_errno(path);    /* ENOENT: the last reference went with the framebuffer */
	}
	if (fd >= 0) {
		close(fd);
	}
	if (fb != 0u) {
		(void)drmModeRmFB(P.card, fb);
	}
	if (kh != 0u) {
		gem_close(P.card, kh);
	}
	rbo_free(&b);
	if (refused && (strcmp(path, "-") != 0)) {
		gone = name_open_errno(path);
	}
	ok = (rc_imp == 0) && (rc_imp2 == 0) && (kh2 == handles[0]) && (rc_uif != 0) && (e_uif == EINVAL) && (rc_pitch != 0) &&
		(e_pitch == EINVAL) && (rc_fb == 0) && shown && (alive == 0) && back && (rc_rm == 0) && (gone == ENOENT);
	printf(TAG "prime_import_card0 export=%d errno=%d path=%s import=%d import_errno=%d reimport_same=%d handle=%u "
		"uif_errno=%d short_pitch_errno=%d addfb=%d addfb_errno=%d shown=%d alive_while_shown=%d flipped_off=%d rmfb=%d "
		"gone_after_errno=%d%s ok=%d\n", rc_exp, e, path, rc_imp, e_imp, (rc_imp2 == 0) && (kh2 == handles[0]), handles[0],
		e_uif, e_pitch, rc_fb, e_fb, shown, alive == 0, back, rc_rm, gone,
		refused ? " gap=1 (ADDFB2 refused: the buffer is not scan-out capable, see the KMS fb FAIL line)" : "", ok);
	if (refused && (rc_imp == 0) && (e_uif == EINVAL) && (e_pitch == EINVAL) && (gone == ENOENT)) {
		P.gap++;
	}
	else {
		verdict("prime_import_card0", ok);
	}
}


/* The refusals: descriptors that are not buffers, and a framebuffer larger than the
 * imported buffer. */
static void t_prime_card0_neg(void)
{
	char path[64] = "-";
	uint32_t h = 0, kh = 0, fb = 0, handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	uint64_t mods[4] = { 0 };
	rbo_t b;
	int rc, e_bad = 0, e_notbuf = 0, fd = -1, rc_imp = -1, e_imp = 0, rc_fb = 0, e_fb = 0, ok;

	rc = drmPrimeFDToHandle(P.card, 1000, &h);   /* no such descriptor */
	e_bad = (rc != 0) ? errno : 0;
	rc = drmPrimeFDToHandle(P.card, P.render, &h);   /* a descriptor that is not a buffer */
	e_notbuf = (rc != 0) ? errno : 0;
	if (rbo_new(&b, XP_SIZE) == 0) {
		if (drmPrimeHandleToFD(P.render, b.handle, DRM_CLOEXEC, &fd) == 0) {
			(void)sys_fdpath(fd, path, sizeof(path));
			rc_imp = drmPrimeFDToHandle(P.card, fd, &kh);
			e_imp = (rc_imp != 0) ? errno : 0;
		}
		if (rc_imp == 0) {
			handles[0] = kh;
			pitches[0] = 7680u;
			rc_fb = drmModeAddFB2WithModifiers(P.card, 1920u, 1080u, DRM_FORMAT_XRGB8888, handles, pitches, offsets, mods,
				&fb, DRM_MODE_FB_MODIFIERS);   /* 64 KiB buffer, 8 MB framebuffer */
			e_fb = (rc_fb != 0) ? errno : 0;
			if (rc_fb == 0) {
				(void)drmModeRmFB(P.card, fb);
			}
			gem_close(P.card, kh);
		}
		if (fd >= 0) {
			close(fd);
		}
	}
	rbo_free(&b);
	ok = (e_bad == EBADF) && (e_notbuf == EINVAL) && (rc_imp == 0) && (rc_fb != 0) && (e_fb == EINVAL) &&
		(name_open_errno(path) == ENOENT);
	printf(TAG "prime_import_card0_neg badfd_errno=%d notbuf_errno=%d small_import=%d small_import_errno=%d "
		"small_addfb_errno=%d released=%d ok=%d\n", e_bad, e_notbuf, rc_imp, e_imp, e_fb,
		(strcmp(path, "-") != 0) && (name_open_errno(path) == ENOENT), ok);
	verdict("prime_import_card0_neg", ok);
}


/* ========================================================================= */
/* Scan-out placement below 1 GiB (render server proto 5, V3DA_BO_LOWMEM)     */
/* ========================================================================= */

/* A render BO's card0 verdict: export, import on card0, LINEAR XRGB8888 ADDFB2 of
 * w x h at `pitch`, then everything released again. 0 or the ADDFB2 errno; -1 when
 * the buffer never reached ADDFB2. */
static int lowmem_addfb(const rbo_t *b, uint32_t w, uint32_t h, uint32_t pitch)
{
	uint32_t kh = 0, fb = 0, handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	uint64_t mods[4] = { 0 };
	int fd = -1, e = -1;

	if (drmPrimeHandleToFD(P.render, b->handle, DRM_CLOEXEC | DRM_RDWR, &fd) != 0) {
		return -1;
	}
	if (drmPrimeFDToHandle(P.card, fd, &kh) == 0) {
		handles[0] = kh;
		pitches[0] = pitch;
		mods[0] = DRM_FORMAT_MOD_LINEAR;
		e = (drmModeAddFB2WithModifiers(P.card, w, h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, mods, &fb,
			DRM_MODE_FB_MODIFIERS) == 0) ? 0 : errno;
		if (fb != 0u) {
			(void)drmModeRmFB(P.card, fb);
		}
		gem_close(P.card, kh);
	}
	close(fd);
	return e;
}


/* Proto 5: a render BO created with the scan-out placement hint (what Mesa v3d asks
 * for a PIPE_BIND_SCANOUT resource on Phoenix, mesa-drm patch 0016) lies below
 * 1 GiB, so card0 takes it as a LINEAR framebuffer whatever the kernel's allocator
 * does with the render server's other blocks - m6h-g7's `why=above_1g` refusals of
 * client buffers are what it removes. A plain BO of the same size is tried for
 * comparison: its ADDFB2 depends on where its block landed (informational on the Pi;
 * refused when the fake server puts unplaced BOs high). An unknown create flag is
 * EINVAL, as on Linux. The size is the mode's frame plus Mesa's TFU read-ahead page:
 * 2026 pages at 1920x1080, one 8 MiB buddy block, as a client's buffer. */
static void t_scanout_lowmem(void)
{
	uint32_t w = (P.mode.hdisplay != 0u) ? P.mode.hdisplay : 1920u, h = (P.mode.vdisplay != 0u) ? P.mode.vdisplay : 1080u;
	uint32_t pitch = ((w * 4u) + 63u) & ~63u, size = pitch * h + 4096u;
	rbo_t low, plain, bogus;
	int rc_low, rc_plain, rc_bogus, e_bogus, e_low = -1, e_plain = -1, ok;

	rc_bogus = rbo_new_flags_fd(P.render, &bogus, 4096u, 1u << 30);
	e_bogus = (rc_bogus != 0) ? -rc_bogus : 0;
	rbo_free(&bogus);

	rc_low = rbo_new_flags_fd(P.render, &low, size, PROBE_V3D_CREATE_BO_SCANOUT);
	if (rc_low == 0) {
		e_low = lowmem_addfb(&low, w, h, pitch);
	}
	rc_plain = rbo_new(&plain, size);
	if (rc_plain == 0) {
		e_plain = lowmem_addfb(&plain, w, h, pitch);
	}
	rbo_free(&low);
	rbo_free(&plain);
	ok = (e_bogus == EINVAL) && (rc_low == 0) && (e_low == 0);
	printf(TAG "scanout_lowmem %ux%u pages=%u bogus_flag_errno=%d create=%d low_addfb_errno=%d plain_create=%d "
		"plain_addfb_errno=%d%s ok=%d\n", w, h, (size + 4095u) / 4096u, e_bogus, rc_low, e_low, rc_plain, e_plain,
		(e_low == EINVAL) ? " (the placed BO was refused: see the KMS fb FAIL line, why=above_1g)" : "",
		ok);
	verdict("scanout_lowmem", ok);
}


/* ========================================================================= */
/* G6: cross-process implicit sync                                            */
/* ========================================================================= */

/* A dma-buf's implicit fences as a sync file (0 or errno), as Mesa's WSI asks. */
static int g6_export(int dfd, int *sfd)
{
	struct probe_dma_buf_sync_file s = { .flags = PROBE_DMA_BUF_SYNC_RW, .fd = -1 };

	*sfd = -1;
	if (drmIoctl(dfd, PROBE_DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &s) != 0) {
		return errno;
	}
	*sfd = s.fd;
	return 0;
}


static int g6_import(int dfd, int sfd)
{
	struct probe_dma_buf_sync_file s = { .flags = PROBE_DMA_BUF_SYNC_RW, .fd = sfd };

	return (drmIoctl(dfd, PROBE_DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &s) != 0) ? errno : 0;
}


/* SYNC_IOC_FILE_INFO: 1 signalled, 0 active, -errno. No wait. */
static int g6_status(int sfd, unsigned *nfences)
{
	struct probe_sync_file_info fi;

	memset(&fi, 0, sizeof(fi));
	if (ioctl(sfd, PROBE_SYNC_IOC_FILE_INFO, &fi) != 0) {
		return -errno;
	}
	if (nfences != NULL) {
		*nfences = fi.num_fences;
	}
	return fi.status;
}


/* Wait for a sync file as gallium's fence_finish does: import it into a syncobj. */
static int g6_wait(int sfd)
{
	uint32_t s = 0;
	int rc;

	rc = drmSyncobjCreate(P.render, 0, &s);
	if (rc == 0) rc = drmSyncobjImportSyncFile(P.render, s, sfd);
	if (rc == 0) rc = drmSyncobjWait(P.render, &s, 1, INT64_MAX, 0, NULL);
	if (s != 0u) (void)drmSyncobjDestroy(P.render, s);
	return rc;
}


/* Words != want among every step-th word (and the last one). */
static uint32_t g6_count_not_step(const volatile uint32_t *p, uint32_t n, uint32_t want, uint32_t step)
{
	uint32_t i, bad = 0;

	for (i = 0; i < n; i += step) {
		bad += (p[i] != want);
	}
	if ((n != 0u) && (((n - 1u) % step) != 0u)) {
		bad += (p[n - 1u] != want);
	}
	return bad;
}



/* The consumer (a compositor sampling a client buffer): the buffer arrived while
 * its producer's GPU work may still run. Read it at once (what a consumer without
 * sync would see), then export the buffer's implicit fences, wait, read again. */
typedef struct {
	uint32_t early_bad, bad;
	int e_exp, st_export, st_after, wrc;
	unsigned nfences;
} g6_read_t;

static void g6_read_init(g6_read_t *o)
{
	memset(o, 0, sizeof(*o));
	o->e_exp = o->st_export = o->st_after = o->wrc = -1;
	o->early_bad = o->bad = ~0u;
}


static void g6_consume(int dfd, const volatile uint32_t *fm, uint32_t words, uint32_t want, g6_read_t *o)
{
	int sfd = -1;

	g6_read_init(o);
	o->early_bad = g6_count_not_step(fm, words, want, (words > 65536u) ? 61u : 1u);   /* fast: the race is on */
	o->e_exp = g6_export(dfd, &sfd);
	if (o->e_exp == 0) {
		o->st_export = g6_status(sfd, &o->nfences);
		o->wrc = g6_wait(sfd);
		o->st_after = g6_status(sfd, NULL);
		close(sfd);
	}
	o->bad = g6_count_not_step(fm, words, want, (words > 65536u) ? 7u : 1u);   /* after the sync - or right away without */
}


/* Mesa's WSI feature probe (wsi_drm_check_dma_buf_sync_file_import_export): a
 * render-node allocation, its dma-buf, EXPORT_SYNC_FILE, then IMPORT_SYNC_FILE of
 * that sync file. Before G6 both answered ENOTTY and WSI used driver implicit sync. */
static void t_g6_probe(void)
{
	rbo_t b;
	int fd = -1, sfd = -1, e_exp = -1, e_imp = -1, rc, ok;
	unsigned nf = 99u;

	rc = rbo_new(&b, 4096u);
	if ((rc == 0) && (drmPrimeHandleToFD(P.render, b.handle, DRM_CLOEXEC | DRM_RDWR, &fd) == 0)) {
		e_exp = g6_export(fd, &sfd);
		if (e_exp == 0) {
			(void)g6_status(sfd, &nf);
			e_imp = g6_import(fd, sfd);
			close(sfd);
		}
		close(fd);
	}
	rbo_free(&b);
	ok = (e_exp == 0) && (e_imp == 0) && (nf == 0u);
	printf(TAG "dmabuf_sync_probe export_errno=%d import_errno=%d idle_fences=%u ok=%d\n", e_exp, e_imp, nf, ok);
	verdict("dmabuf_sync_probe", ok);
}


/* The producer's GPU work: a chain of k dependent FULL-SCREEN clears (the mode's
 * size, raster RGBA8, stride = width * 4) straight into the buffer that is handed
 * over - k-1 in colour X, the last in Y, each job waiting for the previous one
 * (in_sync_bcl = out_sync) - so the buffer holds Y everywhere only when the whole
 * chain is done, and the chain is long on the GPU itself (each clear stores the
 * whole 8 MB frame), not only in submission IPC. g6-sync (2026-09-27) showed why:
 * a chain of 64x64 clears finished faster than it could be submitted, and every
 * "pending" check read 0. */
#define G6_X 0xff4040c0u
#define G6_Y 0xff20c060u
#define G6_SENTINEL 0xdeadbeefu

typedef struct {
	int fd;
	uint32_t w, h, words;
	int rt_borrowed;                 /* rt is the caller's buffer: not freed with the chain */
	rbo_t rt, bcl[2], rcl[2], ta, ts;
	struct drm_v3d_submit_cl s[2];
	uint32_t sync;
	uint64_t t_start, t_submitted;   /* around the submit loop */
} g6_chain_t;

static uint32_t g6_page(uint32_t n)
{
	return (n + 4095u) & ~4095u;
}


static void g6_dims(uint32_t *w, uint32_t *h)
{
	*w = (P.mode.hdisplay != 0u) ? P.mode.hdisplay : 1920u;
	*h = (P.mode.vdisplay != 0u) ? P.mode.vdisplay : 1080u;
}


/* rt == NULL: the chain renders into a scan-out BO of its own; else into the
 * caller's (mapped) buffer, which must hold the whole mode-sized frame. */
static int g6_chain_setup_rt(int fd, g6_chain_t *ch, const rbo_t *rt)
{
	uint32_t bsz, rsz, tasz, tssz, i;
	v3da_clgen_buf_t gb, gr;
	int rc;

	memset(ch, 0, sizeof(*ch));
	ch->fd = fd;
	g6_dims(&ch->w, &ch->h);
	ch->words = ch->w * ch->h;
	if ((V3DA_CLGEN_CLEAR_RT_STRIDE(ch->w) & 63u) != 0u) {
		return -EINVAL;   /* the scan-out pitch must be the clear's stride (1920 * 4 is 64-aligned) */
	}
	rc = v3da_clgen_clear_sizes(ch->w, ch->h, &bsz, &rsz, &tasz, &tssz);
	if ((rc == 0) && (rt != NULL)) {
		ch->rt = *rt;
		ch->rt_borrowed = 1;
		rc = ((rt->cpu != NULL) && (rt->size >= V3DA_CLGEN_CLEAR_RT_SIZE(ch->w, ch->h))) ? 0 : -EINVAL;
	}
	/* the render target is the buffer that is scanned out: ask for the scan-out
	 * placement, as Mesa does (proto 5 places it below 1 GiB; older servers never see the hint) */
	else if (rc == 0) {
		rc = rbo_new_flags_fd(fd, &ch->rt, g6_page(V3DA_CLGEN_CLEAR_RT_SIZE(ch->w, ch->h)), PROBE_V3D_CREATE_BO_SCANOUT);
	}
	for (i = 0; (rc == 0) && (i < 2u); i++) {
		rc = rbo_new_fd(fd, &ch->bcl[i], g6_page(bsz));
		if (rc == 0) rc = rbo_new_fd(fd, &ch->rcl[i], g6_page(rsz));
	}
	if (rc == 0) rc = rbo_new_fd(fd, &ch->ta, tasz);
	if (rc == 0) rc = rbo_new_fd(fd, &ch->ts, g6_page(tssz));
	for (i = 0; (rc == 0) && (i < 2u); i++) {
		gb.cpu = ch->bcl[i].cpu;
		gb.gpuva = ch->bcl[i].offset;
		gb.size = ch->bcl[i].size;
		gr.cpu = ch->rcl[i].cpu;
		gr.gpuva = ch->rcl[i].offset;
		gr.size = ch->rcl[i].size;
		rc = v3da_clgen_clear(ch->w, ch->h, ch->rt.offset, (i == 0u) ? G6_X : G6_Y, &gb, &gr, ch->ta.offset, ch->ta.size,
			ch->ts.offset, &ch->s[i]);
	}
	if (rc == 0) rc = drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, &ch->sync);
	return rc;
}


static int g6_chain_setup(int fd, g6_chain_t *ch)
{
	return g6_chain_setup_rt(fd, ch, NULL);
}


static int g6_chain_submit(g6_chain_t *ch, uint32_t k)
{
	struct drm_v3d_submit_cl s;
	uint32_t handles[5], j, i;

	for (j = 0; j < ch->words; j++) {
		ch->rt.cpu[j] = G6_SENTINEL;   /* "not rendered yet" */
	}
	ch->t_start = now_us();
	for (j = 0; j < k; j++) {
		i = (j + 1u == k) ? 1u : 0u;
		s = ch->s[i];
		handles[0] = ch->rt.handle;
		handles[1] = ch->bcl[i].handle;
		handles[2] = ch->rcl[i].handle;
		handles[3] = ch->ta.handle;
		handles[4] = ch->ts.handle;
		s.bo_handles = (uintptr_t)handles;
		s.bo_handle_count = 5u;
		s.in_sync_bcl = ch->sync;
		s.out_sync = ch->sync;
		if (drmIoctl(ch->fd, DRM_IOCTL_V3D_SUBMIT_CL, &s) != 0) {
			return -errno;
		}
	}
	ch->t_submitted = now_us();
	return 0;
}


/* How long the GPU still worked after the last submit: a blocking wait on the
 * chain's syncobj (the server answers it at the fence's completion IRQ). */
static uint64_t g6_chain_tail_us(g6_chain_t *ch)
{
	(void)drmSyncobjWait(ch->fd, &ch->sync, 1, INT64_MAX, 0, NULL);
	return now_us() - ch->t_submitted;
}


static void g6_chain_free(g6_chain_t *ch)
{
	uint32_t i;

	if (ch->sync != 0u) {
		(void)drmSyncobjDestroy(ch->fd, ch->sync);
	}
	if (!ch->rt_borrowed) {
		rbo_free_fd(ch->fd, &ch->rt);
	}
	for (i = 0; i < 2u; i++) {
		rbo_free_fd(ch->fd, &ch->bcl[i]);
		rbo_free_fd(ch->fd, &ch->rcl[i]);
	}
	rbo_free_fd(ch->fd, &ch->ta);
	rbo_free_fd(ch->fd, &ch->ts);
	memset(ch, 0, sizeof(*ch));
}


/* A race the probe did not provoke proves nothing: the producer was already done
 * when the consumer looked, so a no-op sync would pass too. Such a key fails as
 * INCONCLUSIVE (re-run with a larger -g). */
#define G6_INCONCLUSIVE " inconclusive=1 (the producer was done before the consumer looked: the race was not provoked; re-run with a larger -g)"


/* IMPORT_SYNC_FILE: this process's own pending GPU work (a job chain) attached to a
 * shared buffer it did not render into; the buffer's exported fences must then
 * include it, and waiting for them must mean the chain is done. */
static void t_g6_import(void)
{
	g6_chain_t ch;
	rbo_t sh;
	int rc, fd = -1, sfd = -1, sfd2 = -1, e_imp = -1, e_exp = -1, st = -1, wrc = -1, own_done = 0, ok, inconcl = 0;
	unsigned nf = 0u;
	uint64_t tail = 0;

	memset(&sh, 0, sizeof(sh));
	rc = g6_chain_setup(P.render, &ch);
	if (rc == 0) rc = rbo_new(&sh, 4096u);
	if (rc == 0) rc = drmPrimeHandleToFD(P.render, sh.handle, DRM_CLOEXEC | DRM_RDWR, &fd);
	if (rc == 0) rc = g6_chain_submit(&ch, P.g6_jobs);
	if (rc == 0) rc = drmSyncobjExportSyncFile(P.render, ch.sync, &sfd);
	if (rc == 0) {
		e_imp = g6_import(fd, sfd);
		e_exp = g6_export(fd, &sfd2);
		if (e_exp == 0) {
			st = g6_status(sfd2, &nf);   /* 0 while the chain runs: the buffer now waits for it */
			wrc = g6_wait(sfd2);
			tail = now_us() - ch.t_submitted;
			close(sfd2);
		}
		own_done = (drmSyncobjWait(P.render, &ch.sync, 1, 0, 0, NULL) == 0);   /* poll: no wait */
	}
	if (sfd >= 0) close(sfd);
	if (fd >= 0) close(fd);
	rbo_free(&sh);
	g6_chain_free(&ch);
	ok = (rc == 0) && (e_imp == 0) && (e_exp == 0) && (wrc == 0) && own_done && (nf >= 1u || st != 0);
	inconcl = ok && (st != 0);
	ok = ok && (st == 0);
	printf(TAG "dmabuf_sync_import setup=%d import_errno=%d reexport_errno=%d pending_after_import=%d nfences=%u wait=%d "
		"chain_done=%d jobs=%u tail_us=%llu%s ok=%d\n", rc, e_imp, e_exp, st == 0, nf, wrc, own_done, P.g6_jobs,
		(unsigned long long)tail, inconcl ? G6_INCONCLUSIVE : "", ok);
	verdict("dmabuf_sync_import", ok);
}


/* A scan-out sized render BO, exported and imported on card0 as a LINEAR XRGB8888
 * framebuffer (the G7 path). */
typedef struct {
	rbo_t b;
	int dfd;
	uint32_t kh, fb, w, h, pitch;
	int e_fb;
} g6_scan_t;

static int g6_scan_fb(int dfd, g6_scan_t *sc)
{
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	uint64_t mods[4] = { DRM_FORMAT_MOD_LINEAR, 0, 0, 0 };

	if (drmPrimeFDToHandle(P.card, dfd, &sc->kh) != 0) {
		return -errno;
	}
	handles[0] = sc->kh;
	pitches[0] = sc->pitch;
	if (drmModeAddFB2WithModifiers(P.card, sc->w, sc->h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, mods, &sc->fb,
			DRM_MODE_FB_MODIFIERS) != 0) {
		sc->e_fb = errno;
		return -sc->e_fb;
	}
	return 0;
}


static void g6_scan_geometry(g6_scan_t *sc)
{
	memset(sc, 0, sizeof(*sc));
	sc->dfd = -1;
	g6_dims(&sc->w, &sc->h);
	sc->pitch = V3DA_CLGEN_CLEAR_RT_STRIDE(sc->w);   /* the producer's clear writes exactly this layout */
}


/* Flip the imported buffer with NO in-fence (as Weston flips a client buffer on a
 * plane): the flip must not complete before the buffer's producer is done. At the
 * flip event the pixels are read back (sampled): they must be the producer's last
 * colour, the frame the plane now shows. */
typedef struct {
	int st_commit, st_flip, flipped, back, rmfb;
	uint64_t flip_us;
	uint32_t bad_at_flip;
} g6_flip_t;

static void g6_flip_init(g6_flip_t *o)
{
	memset(o, 0, sizeof(*o));
	o->st_commit = o->st_flip = o->rmfb = -1;
	o->bad_at_flip = ~0u;
}


static void g6_flip(g6_scan_t *sc, g6_flip_t *o, const volatile uint32_t *px, uint32_t words, uint32_t want)
{
	uint64_t t0;
	int sfd = -1, rc;

	g6_flip_init(o);
	if (g6_export(sc->dfd, &sfd) == 0) {
		o->st_commit = g6_status(sfd, NULL);   /* 0: the producer still runs as the flip is asked for */
		close(sfd);
	}
	P.flips_done = 0;
	t0 = now_us();
	rc = drmModePageFlip(P.card, P.crtc, sc->fb, DRM_MODE_PAGE_FLIP_EVENT, NULL);
	o->flipped = (rc == 0) && (wait_flips(1, 5000) == 0);
	o->flip_us = now_us() - t0;
	if (o->flipped && (px != NULL)) {
		o->bad_at_flip = g6_count_not_step(px, words, want, 61u);
	}
	if (g6_export(sc->dfd, &sfd) == 0) {
		o->st_flip = g6_status(sfd, NULL);   /* 1: the producer was done when the buffer went on screen */
		close(sfd);
	}
	if (o->flipped) {
		usleep(1000000);   /* the frame on HDMI for a dense snapshot */
		P.flips_done = 0;
		rc = drmModePageFlip(P.card, P.crtc, P.buf[0].fb, DRM_MODE_PAGE_FLIP_EVENT, NULL);
		o->back = (rc == 0) && (wait_flips(1, 3000) == 0);
	}
	o->rmfb = drmModeRmFB(P.card, sc->fb);
	sc->fb = 0;
	gem_close(P.card, sc->kh);
	sc->kh = 0;
}


#ifdef DRMPROBE_NO_FORK
/* Host harness: "another process" is the fake server's foreign job (mock/fake.c): a
 * pending job of a client this library does not know, rendering into our export.
 * This process's mirrors know nothing of it - exactly the cross-process case. */
void drmprobe_host_foreign_job(uint32_t handle, uint32_t colour);
int drmprobe_host_foreign_done(void);

#define G6_HOST_W 64u
#define G6_HOST_H 64u

static void t_g6_read_host(void)
{
	rbo_t rt;
	g6_read_t rd;
	uint32_t *fm = NULL, i;
	int fd = -1, rc, done = 0, ok;

	g6_read_init(&rd);
	rc = rbo_new(&rt, G6_HOST_W * G6_HOST_H * 4u);
	if (rc == 0) {
		for (i = 0; i < G6_HOST_W * G6_HOST_H; i++) {
			rt.cpu[i] = G6_SENTINEL;
		}
		rc = drmPrimeHandleToFD(P.render, rt.handle, DRM_CLOEXEC | DRM_RDWR, &fd);
	}
	if (rc == 0) {
		fm = mmap(NULL, rt.size, PROT_READ, MAP_SHARED, fd, 0);
		fm = (fm == MAP_FAILED) ? NULL : fm;
		rc = (fm != NULL) ? 0 : -1;
	}
	if (rc == 0) {
		drmprobe_host_foreign_job(rt.handle, G6_Y);
		g6_consume(fd, fm, G6_HOST_W * G6_HOST_H, G6_Y, &rd);
		done = drmprobe_host_foreign_done();
	}
	if (fm != NULL) (void)munmap(fm, rt.size);
	if (fd >= 0) close(fd);
	rbo_free(&rt);
	ok = (rc == 0) && (rd.e_exp == 0) && (rd.wrc == 0) && (rd.st_after == 1) && (rd.bad == 0u) && done;
	printf(TAG "dmabuf_sync_read producer=foreign export_errno=%d pending_at_export=%d nfences=%u wait=%d early_stale=%d "
		"bad_words=%u done_at_read=%d%s ok=%d\n", rd.e_exp, rd.st_export == 0, rd.nfences, rd.wrc, rd.early_bad != 0u, rd.bad,
		done, (ok && (rd.st_export != 0)) ? G6_INCONCLUSIVE : "", ok && (rd.st_export == 0));
	verdict("dmabuf_sync_read", ok && (rd.st_export == 0));
}


static void t_g6_flip_host(void)
{
	g6_scan_t sc;
	g6_flip_t fl;
	int rc, done = 0, refused = 0, ok;

	g6_flip_init(&fl);
	g6_scan_geometry(&sc);
	rc = rbo_new(&sc.b, g6_page(sc.pitch * sc.h));
	if (rc == 0) {
		for (uint32_t i = 0; i < sc.w * sc.h; i++) {
			sc.b.cpu[i] = G6_SENTINEL;
		}
		rc = drmPrimeHandleToFD(P.render, sc.b.handle, DRM_CLOEXEC | DRM_RDWR, &sc.dfd);
	}
	if (rc == 0) {
		rc = g6_scan_fb(sc.dfd, &sc);
		refused = (rc == -EINVAL);   /* above 1 GiB (FAKE_KMS_IMPORT_HIGH): not scan-out capable */
	}
	if (rc == 0) {
		drmprobe_host_foreign_job(sc.b.handle, G6_Y);
		g6_flip(&sc, &fl, sc.b.cpu, sc.w * sc.h, G6_Y);
		done = drmprobe_host_foreign_done();
	}
	else if (sc.kh != 0u) {
		gem_close(P.card, sc.kh);
	}
	if (sc.dfd >= 0) close(sc.dfd);
	rbo_free(&sc.b);
	ok = (rc == 0) && fl.flipped && fl.back && (fl.rmfb == 0) && done && (fl.bad_at_flip == 0u);
	printf(TAG "dmabuf_sync_flip producer=foreign addfb=%d pending_at_commit=%d flipped=%d flip_us=%llu done_at_flip=%d "
		"bad_words=%u flipped_back=%d%s%s ok=%d\n", rc, fl.st_commit == 0, fl.flipped, (unsigned long long)fl.flip_us, done,
		fl.bad_at_flip, fl.back, refused ? " gap=1 (ADDFB2 refused: not scan-out capable)" : "",
		(ok && (fl.st_commit != 0)) ? G6_INCONCLUSIVE : "", ok && (fl.st_commit == 0));
	if (refused) {
		P.gap++;
	}
	else {
		verdict("dmabuf_sync_flip", ok && (fl.st_commit == 0));
	}
}

#else /* !DRMPROBE_NO_FORK */

/* The producer process (a Wayland client): its own render node, one scan-out sized
 * buffer, exported once. Round 1: a job chain into it, the dma-buf sent the moment
 * the chain is submitted (cmd 11). The consumer reads, imports the buffer on card0
 * and says so (12). Round 2: a second chain, announced as submitted (13); the
 * consumer flips the buffer and says so (14). Then the producer's own timings (15).
 * Timings: sub = the submit loop, tail = last submit -> the chain's fence signalled
 * (a blocking syncobj wait). tail is the race window the consumer had. */
static void g6_child(int s)
{
	g6_chain_t ch;
	xp_msg_t m, in;
	uint64_t sub1 = 0, tail1 = 0, sub2 = 0, tail2 = 0;
	int rfd, rc, dfd = -1;

	memset(&ch, 0, sizeof(ch));
	rfd = open(P.render_path, O_RDWR | O_CLOEXEC);
	rc = (rfd >= 0) ? g6_chain_setup(rfd, &ch) : -errno;
	if (rc == 0) rc = drmPrimeHandleToFD(rfd, ch.rt.handle, DRM_CLOEXEC | DRM_RDWR, &dfd);

	/* round 1: render, hand over at once */
	if (rc == 0) rc = g6_chain_submit(&ch, P.g6_jobs);
	memset(&m, 0, sizeof(m));
	m.cmd = 11;
	m.rc = rc;
	(void)xp_send(s, &m, (rc == 0) ? dfd : -1);
	if (rc == 0) {
		sub1 = ch.t_submitted - ch.t_start;
		tail1 = g6_chain_tail_us(&ch);
	}
	if (xp_recv(s, &in, 12, NULL) == 0) {
		/* round 2: render again, announce it */
		if (rc == 0) rc = g6_chain_submit(&ch, P.g6_jobs);
		memset(&m, 0, sizeof(m));
		m.cmd = 13;
		m.rc = rc;
		(void)xp_send(s, &m, -1);
		if (rc == 0) {
			sub2 = ch.t_submitted - ch.t_start;
			tail2 = g6_chain_tail_us(&ch);
		}
		(void)xp_recv(s, &in, 14, NULL);
	}
	memset(&m, 0, sizeof(m));
	m.cmd = 15;
	m.rc = rc;
	m.handle = P.g6_jobs;
	m.aux[0] = sub1;
	m.aux[1] = tail1;
	m.aux[2] = sub2;
	m.aux[3] = tail2;
	(void)xp_send(s, &m, -1);
	if (dfd >= 0) close(dfd);
	if (rfd >= 0) {
		g6_chain_free(&ch);
		close(rfd);
	}
}


/* Two processes, as a Wayland client and a compositor. Round 1 (a compositor
 * sampling a client buffer): the consumer maps the dma-buf the moment it arrives,
 * exports its implicit fences, waits, and must then read the producer's LAST colour.
 * Round 2 (direct scan-out): the consumer, with the buffer already imported on card0
 * and added as a framebuffer, flips it with no in-fence the moment the producer's
 * second chain is submitted; libdrm-phoenix asks the render server for the
 * producer's fence and rpi4-kms -G holds the flip until it signals. */
static void t_g6_xproc(void)
{
	g6_read_t rd;
	g6_scan_t sc;
	g6_flip_t fl;
	xp_msg_t q, r;
	uint32_t *fm = NULL;
	pid_t pid = -1;
	int sv[2] = { -1, -1 }, got1 = -1, got2 = -1, got3 = -1, status = 0, rc_fb = -1, refused = 0, ok, pend;
	size_t len;

	g6_read_init(&rd);
	g6_flip_init(&fl);
	memset(&r, 0, sizeof(r));
	g6_scan_geometry(&sc);
	len = g6_page(sc.pitch * sc.h);
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
		fflush(stdout);
		pid = fork();
		if (pid == 0) {
			close(sv[0]);
			g6_child(sv[1]);
			_exit(0);
		}
		close(sv[1]);
		sv[1] = -1;
	}
	if (pid > 0) {
		got1 = xp_recv(sv[0], &r, 11, &sc.dfd);
		if ((got1 == 0) && (r.rc == 0) && (sc.dfd >= 0)) {
			fm = mmap(NULL, len, PROT_READ, MAP_SHARED, sc.dfd, 0);
			fm = (fm == MAP_FAILED) ? NULL : fm;
		}
		if (fm != NULL) {
			g6_consume(sc.dfd, fm, sc.w * sc.h, G6_Y, &rd);
			rc_fb = g6_scan_fb(sc.dfd, &sc);   /* before round 2: the race window is the flip's alone */
			refused = (rc_fb == -EINVAL);
			if ((rc_fb != 0) && (sc.kh != 0u)) {
				gem_close(P.card, sc.kh);
				sc.kh = 0;
			}
		}
		memset(&q, 0, sizeof(q));
		q.cmd = 12;
		(void)xp_send(sv[0], &q, -1);

		got2 = xp_recv(sv[0], &r, 13, NULL);
		if ((got2 == 0) && (r.rc == 0) && (rc_fb == 0)) {
			g6_flip(&sc, &fl, fm, sc.w * sc.h, G6_Y);
		}
		q.cmd = 14;
		(void)xp_send(sv[0], &q, -1);
		got3 = xp_recv(sv[0], &r, 15, NULL);
		(void)waitpid(pid, &status, 0);
	}
	if (fm != NULL) (void)munmap(fm, len);
	if (sc.dfd >= 0) close(sc.dfd);

	ok = (got1 == 0) && (rd.e_exp == 0) && (rd.wrc == 0) && (rd.st_after == 1) && (rd.bad == 0u) && (got3 == 0);
	pend = (rd.st_export == 0);
	printf(TAG "dmabuf_sync_read producer=child jobs=%u size=%ux%u submit_us=%llu tail_us=%llu export_errno=%d "
		"pending_at_export=%d nfences=%u wait=%d early_stale=%d bad_words=%u done_at_read=%d%s ok=%d\n", P.g6_jobs, sc.w, sc.h,
		(unsigned long long)r.aux[0], (unsigned long long)r.aux[1], rd.e_exp, pend, rd.nfences, rd.wrc, rd.early_bad != 0u,
		rd.bad, rd.st_after == 1, (ok && !pend) ? G6_INCONCLUSIVE : "", ok && pend);
	verdict("dmabuf_sync_read", ok && pend);
	ok = (got2 == 0) && (rc_fb == 0) && fl.flipped && (fl.st_flip == 1) && (fl.bad_at_flip == 0u) && fl.back &&
		(fl.rmfb == 0) && (got3 == 0);
	pend = (fl.st_commit == 0);
	printf(TAG "dmabuf_sync_flip producer=child jobs=%u submit_us=%llu tail_us=%llu addfb=%d pending_at_commit=%d flipped=%d "
		"flip_us=%llu done_at_flip=%d bad_at_flip=%u flipped_back=%d report=%d%s%s ok=%d\n", P.g6_jobs,
		(unsigned long long)r.aux[2], (unsigned long long)r.aux[3], rc_fb, pend, fl.flipped, (unsigned long long)fl.flip_us,
		fl.st_flip == 1, fl.bad_at_flip, fl.back, got3 == 0,
		refused ? " gap=1 (ADDFB2 refused: not scan-out capable, see the KMS fb FAIL line)" : "",
		(ok && !pend) ? G6_INCONCLUSIVE : "", ok && pend);
	if (refused) {
		P.gap++;
	}
	else {
		verdict("dmabuf_sync_flip", ok && pend);
	}
	if (sv[0] >= 0) close(sv[0]);
}
#endif /* DRMPROBE_NO_FORK */


/* A wlroots compositor's own output (labwc, sway; G5): the swapchain buffer is a
 * dumb buffer of ANOTHER card0 descriptor (Mesa's kmsro allocates on the card0 open
 * of its GBM device), imported on the render node to be composited into, exported
 * from there (gbm_bo_get_fd) and imported on the backend's card0 descriptor, where it
 * is an alias of the other client's buffer. The backend adds the framebuffer and
 * closes its handle at once (wlroots backend/drm/fb.c close_all_bo_handles: in DRM
 * the framebuffer holds the buffer). The composite is submitted and not waited for,
 * and the atomic flip carries no in-fence (wlroots without explicit sync glFlush()es
 * and commits): the flip must still wait for the composite (G13). Before the G5 fix
 * the closed handle took the framebuffer's buffer name with it, and every such flip
 * went ungated: windowed SuperTuxKart showed the frame from two flips ago. */
static void t_compositor_flip(void)
{
	drmModeObjectPropertiesPtr props;
	drmModeAtomicReqPtr req = NULL;
	struct drm_v3d_get_bo_offset go;
	g6_chain_t ch;
	rbo_t rt;
	uint32_t w, h, dh = 0, pitch = 0, ah = 0, fb = 0, fb_prop = 0, bad = ~0u;
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	uint64_t size = 0, moff = 0, t0, flip_us = 0;
	int gfd, fd = -1, sfd = -1, rc, closed = -1, st_commit = -1, st_flip = -1, flipped = 0, back = 0, rmfb = -1, j, ok, pend;
	char nb[40];

	memset(&ch, 0, sizeof(ch));
	memset(&rt, 0, sizeof(rt));
	g6_dims(&w, &h);
	gfd = open(P.card_path, O_RDWR | O_CLOEXEC);   /* the GBM device's card0: another client */
	rc = (gfd >= 0) ? 0 : -errno;
	if (rc == 0) rc = drmModeCreateDumbBuffer(gfd, w, h, 32, 0, &dh, &pitch, &size);
	if ((rc == 0) && (pitch != V3DA_CLGEN_CLEAR_RT_STRIDE(w))) {
		rc = -EINVAL;   /* the composite (a full-screen clear) writes exactly the clear's layout */
	}
	if (rc == 0) rc = drmModeMapDumbBuffer(gfd, dh, &moff);
	if (rc == 0) {
		rt.cpu = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, gfd, (off_t)moff);
		rc = (rt.cpu != MAP_FAILED) ? 0 : -errno;
		rt.cpu = (rc == 0) ? rt.cpu : NULL;
		rt.size = (uint32_t)size;
	}
	/* kmsro: the render node's view of the scan-out buffer */
	if (rc == 0) rc = drmPrimeHandleToFD(gfd, dh, DRM_CLOEXEC | DRM_RDWR, &fd);
	if (rc == 0) rc = drmPrimeFDToHandle(P.render, fd, &rt.handle);
	if (fd >= 0) {
		close(fd);
		fd = -1;
	}
	if (rc == 0) {
		memset(&go, 0, sizeof(go));
		go.handle = rt.handle;
		rc = (drmIoctl(P.render, DRM_IOCTL_V3D_GET_BO_OFFSET, &go) == 0) ? 0 : -errno;
		rt.offset = go.offset;
	}
	/* the backend: gbm_bo_get_fd, import on its own card0, ADDFB2, close the handle */
	if (rc == 0) rc = drmPrimeHandleToFD(P.render, rt.handle, DRM_CLOEXEC | DRM_RDWR, &fd);
	if (rc == 0) rc = drmPrimeFDToHandle(P.card, fd, &ah);
	if (fd >= 0) {
		close(fd);
	}
	if (rc == 0) {
		handles[0] = ah;
		pitches[0] = pitch;
		rc = drmModeAddFB2(P.card, w, h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &fb, 0);
		rc = (rc == 0) ? 0 : -errno;
	}
	if (rc == 0) {
		closed = drmCloseBufferHandle(P.card, ah);
	}

	/* the composite: pending, not waited for */
	if (rc == 0) rc = g6_chain_setup_rt(P.render, &ch, &rt);
	if (rc == 0) rc = g6_chain_submit(&ch, P.g6_jobs);
	if (rc == 0) rc = drmSyncobjExportSyncFile(P.render, ch.sync, &sfd);
	if (rc == 0) {
		props = drmModeObjectGetProperties(P.card, P.primary, DRM_MODE_OBJECT_PLANE);
		for (j = 0; (props != NULL) && (j < (int)props->count_props); j++) {
			if (strcmp(prop_name_of(P.card, props->props[j], nb, sizeof(nb)), "FB_ID") == 0) {
				fb_prop = props->props[j];
			}
		}
		drmModeFreeObjectProperties(props);
		req = drmModeAtomicAlloc();
		(void)drmModeAtomicAddProperty(req, P.primary, fb_prop, fb);
		st_commit = g6_status(sfd, NULL);   /* 0: the composite still runs as the flip is asked for */
		P.flips_done = 0;
		t0 = now_us();
		flipped = (drmModeAtomicCommit(P.card, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0) &&
			(wait_flips(1, 5000) == 0);
		flip_us = now_us() - t0;
		st_flip = g6_status(sfd, NULL);     /* 1: the composite was done when the buffer went on screen */
		if (flipped) {
			bad = g6_count_not_step(rt.cpu, ch.words, G6_Y, 61u);
		}
		drmModeAtomicFree(req);
		P.flips_done = 0;
		back = (drmModePageFlip(P.card, P.crtc, P.buf[0].fb, DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0) &&
			(wait_flips(1, 3000) == 0);
	}
	if (fb != 0u) {
		rmfb = drmModeRmFB(P.card, fb);
	}
	if (sfd >= 0) {
		(void)g6_wait(sfd);   /* nothing in flight before the buffers go */
		close(sfd);
	}
	g6_chain_free(&ch);
	if (rt.handle != 0u) {
		gem_close(P.render, rt.handle);
	}
	if (rt.cpu != NULL) {
		(void)munmap(rt.cpu, (size_t)size);
	}
	if (dh != 0u) {
		(void)drmModeDestroyDumbBuffer(gfd, dh);
	}
	if (gfd >= 0) {
		close(gfd);
	}

	ok = (rc == 0) && (closed == 0) && flipped && (st_flip == 1) && back && (rmfb == 0);
#ifndef DRMPROBE_NO_FORK
	ok = ok && (bad == 0u);   /* the fake GPU of the host harness draws nothing */
#endif
	pend = (st_commit == 0);
	printf(TAG "compositor_flip setup=%d alias_handle=%u handle_closed=%d jobs=%u pending_at_commit=%d flipped=%d "
		"flip_us=%llu done_at_flip=%d bad_at_flip=%u flipped_back=%d rmfb=%d%s ok=%d\n", rc, ah, closed == 0, P.g6_jobs, pend,
		flipped, (unsigned long long)flip_us, st_flip == 1, bad, back, rmfb, (ok && !pend) ? G6_INCONCLUSIVE : "",
		ok && pend);
	verdict("compositor_flip", ok && pend);
}


static void t_g6(void)
{
	t_g6_probe();
	t_g6_import();
#ifdef DRMPROBE_NO_FORK
	t_g6_read_host();
	if (P.buf[0].handle != 0u) {
		t_g6_flip_host();
	}
#else
	if (P.buf[0].handle != 0u) {
		t_g6_xproc();
	}
	else {
		printf(TAG "dmabuf_sync_read skipped=1 (no KMS: the cross-process test needs the flip half too)\n");
	}
#endif
}


int main(int argc, char **argv)
{
	char card_path[64], render_path[64], card1_path[64];
	int c, nflips = 60, keep = 0, i;
	uint64_t t_start = now_us();

	setvbuf(stdout, NULL, _IOLBF, 0);
	while ((c = getopt(argc, argv, "n:kg:")) != -1) {
		switch (c) {
			case 'n': nflips = atoi(optarg); break;
			case 'k': keep = 1; break;
			case 'g': P.g6_jobs = (atoi(optarg) > 1) ? (uint32_t)atoi(optarg) : 2u; break;
			default:
				printf("usage: %s [-n flips] [-k] [-g jobs]\n", argv[0]);
				return 2;
		}
	}
	printf(TAG "start pid=%d flips=%d libdrm=libdrm-phoenix\n", (int)getpid(), nflips);

	t_devices(card_path, render_path, card1_path, sizeof(card_path));
	if ((card_path[0] == '\0') || (render_path[0] == '\0')) {
		printf(TAG "RESULT pass=%d fail=%d gap=%d failed=%s verdict=FAIL (servers running?)\n", P.pass, P.fail + 1, P.gap,
			P.failed);
		return 1;
	}
	P.card = t_open("card", card_path);
	P.render = t_open("render", render_path);
	(void)snprintf(P.card_path, sizeof(P.card_path), "%s", card_path);
	(void)snprintf(P.render_path, sizeof(P.render_path), "%s", render_path);
	if ((P.card < 0) || (P.render < 0)) {
		printf(TAG "RESULT pass=%d fail=%d gap=%d failed=%s verdict=FAIL\n", P.pass, P.fail, P.gap, P.failed);
		return 1;
	}
	t_identity("card", P.card, "vc4", DRM_NODE_PRIMARY, 1);
	t_identity("render", P.render, "v3d", DRM_NODE_RENDER, 0);
	if ((card1_path[0] != '\0') && (strcmp(card1_path, render_path) != 0)) {
		P.card1 = t_open("card1", card1_path);   /* G10: the v3d primary node has a name (and port) of its own */
		if (P.card1 >= 0) {
			t_identity("card1", P.card1, "v3d", DRM_NODE_PRIMARY, 0);
		}
	}
	else {
		printf(TAG "card1 path=%s distinct=0 gap=1 (no /dev/dri/card1: servers from before M3 part 2, G10)\n",
			(card1_path[0] != '\0') ? card1_path : "-");
		P.gap++;
	}
	t_fstat();
	t_atomic_universal(card_path);

	if (t_kms_enum() == 0) {
		printf(TAG "kms_flip start mode=%ux%u@%u\n", P.mode.hdisplay, P.mode.vdisplay, P.mode.vrefresh);
		if (t_dumb() == 0) {
			t_flips(nflips);
			t_atomic();
		}
	}
	t_render_basics();
	t_render_clear();
	if (P.buf[0].handle != 0u) {
		t_prime();
	}
	t_prime_render();
	if (P.buf[0].handle != 0u) {
		t_prime_card0();
		t_prime_card0_neg();
		t_scanout_lowmem();
	}
#ifndef DRMPROBE_NO_FORK
	t_prime_xproc();
#else
	printf(TAG "prime_export_xproc skipped=1 (DRMPROBE_NO_FORK: host harness)\n");
#endif
	t_g6();
	if ((P.buf[0].handle != 0u) && (P.primary != 0u)) {
		t_compositor_flip();
	}

	if (!keep && (P.crtc != 0u)) {
		(void)drmModeSetCrtc(P.card, P.crtc, 0, 0, 0, NULL, 0, NULL);   /* planes off: the console comes back */
	}
	for (i = 0; i < 2; i++) {
		if (P.buf[i].fb != 0u) {
			(void)drmModeRmFB(P.card, P.buf[i].fb);
		}
		if (P.buf[i].px != NULL) {
			(void)munmap(P.buf[i].px, (size_t)P.buf[i].size);
		}
		if (P.buf[i].handle != 0u) {
			(void)drmModeDestroyDumbBuffer(P.card, P.buf[i].handle);
		}
	}
	if (P.card1 >= 0) {
		close(P.card1);
	}
	close(P.render);
	close(P.card);
	printf(TAG "RESULT pass=%d fail=%d gap=%d failed=%s secs=%llu verdict=%s\n", P.pass, P.fail, P.gap,
		(P.failed[0] != '\0') ? P.failed : "-", (unsigned long long)((now_us() - t_start) / 1000000u),
		(P.fail == 0) ? "PASS" : "FAIL");
	return (P.fail == 0) ? 0 : 1;
}
