/*
 * Phoenix-RTOS
 *
 * libdrm-phoenix - optional ioctl() interposer: the Linux sync_file ioctls on
 * the library's in-process sync files (M5, closes G15 for merge/info), and the
 * dma-buf sync-file ioctls on buffer descriptors (G6)
 *
 * A DRM sync file exported by libdrm-phoenix (SYNCOBJ_HANDLE_TO_FD with
 * EXPORT_SYNC_FILE) is a dup() of the render node descriptor plus a fence set in
 * a process table (M3 section 2.8). Mesa talks to sync files with raw ioctl()s
 * that bypass drmIoctl(): util/libsync.h sync_merge() (SYNC_IOC_MERGE) -- v3dv
 * merges its per-queue fences that way on EVERY vkQueueSubmit that signals a
 * fence or semaphore (v3dv_queue.c merge_syncobjs) -- and sync_valid_fd()
 * (SYNC_IOC_FILE_INFO). Such an ioctl on the dup reaches rpi4-v3d-async, which
 * knows no sync files: the m5-vkcube run hung there. Linked with
 * -Wl,--wrap=ioctl, every ioctl() of the static binary lands here; the two
 * sync_file requests on this process's emulated sync files are answered
 * in-process, everything else goes to the real ioctl() unchanged.
 *
 * This object is its own archive member: it is pulled into a link only by
 * --wrap=ioctl (it references __real_ioctl, which exists only then), so
 * programs linked with --wrap=mmap alone are unaffected.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "libdrm_macros.h"
#include "xf86drm.h"
#include "drm_phoenix_priv.h"

/* The Linux sync_file uapi (linux/sync_file.h), by layout. The request numbers
 * are matched on type + number only, so both the Linux and the Phoenix/BSD _IOC
 * encodings of the same request are recognised (as drmIoctl's dispatch does). */
#define PHX_SYNC_IOC_MAGIC     '>'
#define PHX_SYNC_IOC_MERGE_NR  3u
#define PHX_SYNC_IOC_INFO_NR   4u

struct phx_sync_merge_data {
	char name[32];
	int32_t fd2;
	int32_t fence;
	uint32_t flags;
	uint32_t pad;
};

struct phx_sync_fence_info {
	char obj_name[32];
	char driver_name[32];
	int32_t status;
	uint32_t flags;
	uint64_t timestamp_ns;
};

struct phx_sync_file_info {
	char name[32];
	int32_t status;
	uint32_t flags;
	uint32_t num_fences;
	uint32_t pad;
	uint64_t sync_fence_info;
};

extern int __real_ioctl(int fd, unsigned long request, ...);


/* SYNC_IOC_MERGE / SYNC_IOC_FILE_INFO: type '>', number 3/4 and the struct size
 * (the size field sits at bits 16-29 in both _IOC encodings for these sizes). */
static int is_sync_request(unsigned long request)
{
	unsigned long nr = request & 0xffu, len = (request >> 16) & 0x1fffu;

	if (((request >> 8) & 0xffu) != (unsigned long)PHX_SYNC_IOC_MAGIC) {
		return 0;
	}
	return ((nr == PHX_SYNC_IOC_MERGE_NR) && (len == sizeof(struct phx_sync_merge_data))) ||
		((nr == PHX_SYNC_IOC_INFO_NR) && (len == sizeof(struct phx_sync_file_info)));
}
drm_public int __wrap_ioctl(int fd, unsigned long request, ...);


static int sync_ioctl(int fd, unsigned nr, void *arg)
{
	int rc;

	if (nr == PHX_SYNC_IOC_MERGE_NR) {
		struct phx_sync_merge_data *m = arg;
		if ((m == NULL) || (m->flags != 0u) || (m->pad != 0u)) {
			return -EINVAL;
		}
		rc = drmphx_syncfile_merge(fd, m->fd2);
		if (rc < 0) {
			return rc;
		}
		m->fence = rc;
		return 0;
	}
	if (nr == PHX_SYNC_IOC_INFO_NR) {
		struct phx_sync_file_info *info = arg;
		struct phx_sync_fence_info *fi;
		uint32_t n = 0u, i;
		if ((info == NULL) || (info->flags != 0u) || (info->pad != 0u)) {
			return -EINVAL;
		}
		rc = drmphx_syncfile_status(fd, &n);
		if (rc < 0) {
			return rc;
		}
		(void)snprintf(info->name, sizeof(info->name), "libdrm-phoenix");
		info->status = rc;   /* 1 signalled, 0 active (per-fence errors never happen here) */
		/* Linux: num_fences == 0 asks for the count only; else fill up to it. */
		if ((info->num_fences != 0u) && (info->sync_fence_info != 0u)) {
			fi = (struct phx_sync_fence_info *)(uintptr_t)info->sync_fence_info;
			for (i = 0u; (i < n) && (i < info->num_fences); i++) {
				memset(&fi[i], 0, sizeof(fi[i]));
				(void)snprintf(fi[i].obj_name, sizeof(fi[i].obj_name), "v3da-fence");
				(void)snprintf(fi[i].driver_name, sizeof(fi[i].driver_name), "v3d");
				fi[i].status = rc;   /* the set's status: per-fence status is not tracked */
			}
		}
		info->num_fences = n;
		return 0;
	}
	return -ENOTTY;   /* other sync_file requests do not exist on this emulation */
}


drm_public int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;
	int rc;

	va_start(ap, request);
	arg = va_arg(ap, void *);   /* every caller passes one pointer (or nothing: then unused) */
	va_end(ap);

	if ((((request >> 8) & 0xffu) == (unsigned long)DRMPHX_DMA_BUF_BASE) && (((request >> 16) & 0x1fffu) == 8u) &&
			(((request & 0xffu) == DRMPHX_DMA_BUF_EXPORT_NR) || ((request & 0xffu) == DRMPHX_DMA_BUF_IMPORT_NR)) &&
			(drmphx_prime_fd_lookup(fd, &(kms_memref_t){ 0 }) == 0)) {
		/* G6: DMA_BUF_IOCTL_EXPORT/IMPORT_SYNC_FILE issued with raw ioctl() on a buffer
		 * descriptor (drmIoctl callers reach the same code through drm_phoenix_ioctl) */
		rc = drmphx_dmabuf_ioctl(fd, (unsigned)(request & 0xffu), arg);
		if (rc != 0) {
			errno = -rc;
			return -1;
		}
		return 0;
	}
	if (is_sync_request(request) != 0) {
		/* Phoenix has no kernel sync files: on any other descriptor the request would
		 * go to that descriptor's server as an unknown ioctl (on the render node it
		 * never came back in m5-vkcube). Answer it here, as Linux does for a
		 * descriptor that is not a sync file. */
		rc = (drmphx_syncfile_is(fd) != 0) ? sync_ioctl(fd, (unsigned)(request & 0xffu), arg) : -ENOTTY;
		if (drmphx_trace_enabled() != 0) {
			char line[128];
			int len = snprintf(line, sizeof(line), "DRMPHX sync  fd=%d nr=%u rc=%d%s%d\n", fd, (unsigned)(request & 0xffu),
				rc, ((request & 0xffu) == PHX_SYNC_IOC_MERGE_NR) ? " merged_fd=" : " status=",
				(rc != 0) ? -1 : (((request & 0xffu) == PHX_SYNC_IOC_MERGE_NR) ? ((struct phx_sync_merge_data *)arg)->fence :
					((struct phx_sync_file_info *)arg)->status));
			if (len > 0) {
				(void)write(2, line, ((size_t)len < sizeof(line)) ? (size_t)len : sizeof(line) - 1u);
			}
		}
		if (rc != 0) {
			errno = -rc;
			return -1;
		}
		return 0;
	}
	return __real_ioctl(fd, request, arg);
}
