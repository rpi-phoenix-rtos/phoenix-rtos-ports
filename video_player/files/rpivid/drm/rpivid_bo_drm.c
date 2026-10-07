/*
 * Phoenix-RTOS
 *
 * hevc_rpivid zero-copy output: picture buffers from the V3D render server (see
 * rpivid_bo_drm.h)
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
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>

#include <xf86drm.h>

#include <libavcodec/rpivid_drm.h>

#include "rpivid_bo_drm.h"

/* The V3D BO ioctls (Linux include/uapi/drm/v3d_drm.h, MIT; the render server serves them) */
struct rpivid_v3d_wait_bo {
	uint32_t handle;
	uint32_t pad;
	uint64_t timeout_ns;
};

struct rpivid_v3d_create_bo {
	uint32_t size;
	uint32_t flags;
	uint32_t handle;
	uint32_t offset;
};

struct rpivid_v3d_mmap_bo {
	uint32_t handle;
	uint32_t flags;
	uint64_t offset;
};

#define RPIVID_V3D_WAIT_BO   DRM_IOWR(DRM_COMMAND_BASE + 0x01, struct rpivid_v3d_wait_bo)
#define RPIVID_V3D_CREATE_BO DRM_IOWR(DRM_COMMAND_BASE + 0x02, struct rpivid_v3d_create_bo)
#define RPIVID_V3D_MMAP_BO   DRM_IOWR(DRM_COMMAND_BASE + 0x03, struct rpivid_v3d_mmap_bo)

#define PAGE 4096u

static int render_fd = -1;
static pthread_mutex_t install_lock = PTHREAD_MUTEX_INITIALIZER;


static void bo_close(uint32_t handle)
{
	struct drm_gem_close gc;

	memset(&gc, 0, sizeof(gc));
	gc.handle = handle;
	(void)drmIoctl(render_fd, DRM_IOCTL_GEM_CLOSE, &gc);
}


static int bo_alloc(void *opaque, size_t size, RpividDrmBuffer *b)
{
	struct rpivid_v3d_create_bo cb;
	struct rpivid_v3d_mmap_bo mb;
	void *p;
	size_t i;
	int fd = -1;

	(void)opaque;
	size = (size + PAGE - 1u) & ~(size_t)(PAGE - 1u);
	if (size > UINT32_MAX) {
		return -EINVAL;
	}
	memset(&cb, 0, sizeof(cb));
	cb.size = (uint32_t)size;
	if (drmIoctl(render_fd, RPIVID_V3D_CREATE_BO, &cb) != 0) {
		return -errno;
	}
	memset(&mb, 0, sizeof(mb));
	mb.handle = cb.handle;
	if (drmIoctl(render_fd, RPIVID_V3D_MMAP_BO, &mb) != 0) {
		bo_close(cb.handle);
		return -errno;
	}
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, render_fd, (off_t)mb.offset);
	if (p == MAP_FAILED) {
		bo_close(cb.handle);
		return -ENOMEM;
	}
	/* the block has no MMU: one physically contiguous range, or the BO is refused */
	b->pa = (uint64_t)va2pa(p);
	for (i = PAGE; (b->pa != 0u) && (i < size); i += PAGE) {
		if ((uint64_t)va2pa((uint8_t *)p + i) != b->pa + i) {
			b->pa = 0;
		}
	}
	if ((b->pa == 0u) || (drmPrimeHandleToFD(render_fd, cb.handle, DRM_CLOEXEC, &fd) != 0)) {
		munmap(p, size);
		bo_close(cb.handle);
		return (b->pa == 0u) ? -EFAULT : -errno;
	}
	b->cpu = p;
	b->size = size;
	b->fd = fd;
	b->handle = cb.handle;
	b->priv = NULL;
	return 0;
}


static void bo_free(void *opaque, RpividDrmBuffer *b)
{
	(void)opaque;
	if (b->cpu != NULL) {
		munmap(b->cpu, b->size);
	}
	if (b->fd >= 0) {
		close(b->fd);
	}
	bo_close(b->handle);
	memset(b, 0, sizeof(*b));
	b->fd = -1;
}


static uint64_t now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}


static int bo_wait_idle(void *opaque, const RpividDrmBuffer *b)
{
	struct rpivid_v3d_wait_bo wb;
	uint64_t t0 = now_ms(), dt;
	int i, rc = -ETIMEDOUT;

	(void)opaque;
	/* the server parks a wait for at most 2 s (V3DA_WAIT_MAX_MS): a few tries */
	for (i = 0; i < 5; i++) {
		memset(&wb, 0, sizeof(wb));
		wb.handle = b->handle;
		wb.timeout_ns = 2000000000ull;
		if (drmIoctl(render_fd, RPIVID_V3D_WAIT_BO, &wb) == 0) {
			rc = 0;
			break;
		}
		rc = -errno;
		if ((errno != ETIME) && (errno != ETIMEDOUT) && (errno != EBUSY) && (errno != EINTR)) {
			break;
		}
	}
	/* a picture buffer the GPU held long (a stall to explain): stderr, as the players log */
	dt = now_ms() - t0;
	if ((rc != 0) || (dt > 50u)) {
		fprintf(stderr, "rpivid-bo: WAIT_BO handle %u: %s after %llu ms\n", b->handle, (rc == 0) ? "idle" : strerror(-rc),
			(unsigned long long)dt);
	}
	return rc;
}


int rpivid_bo_drm_install(void)
{
	static const RpividDrmBufferOps ops = { NULL, bo_alloc, bo_free, bo_wait_idle };
	int rc = 0;

	pthread_mutex_lock(&install_lock);
	if (render_fd < 0) {
		render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
		rc = (render_fd < 0) ? -errno : 0;
	}
	if (rc == 0) {
		rpivid_drm_set_buffer_ops(&ops);
	}
	pthread_mutex_unlock(&install_lock);
	return rc;
}
