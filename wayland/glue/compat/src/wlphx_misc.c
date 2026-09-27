/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm compat: small libphoenix gaps (compiled only while libphoenix lacks
 * the symbol; build.sh checks).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>

#ifdef WLPHX_NEED_MSYNC
/* Mappings of Phoenix memory objects are coherent and never written back to a
 * file, so there is nothing to synchronise (libwayland's wl_os_mremap_maymove). */
int msync(void *addr, size_t len, int flags)
{
	(void)len;
	if ((((uintptr_t)addr & 4095u) != 0u) || ((flags & ~(MS_ASYNC | MS_SYNC | MS_INVALIDATE)) != 0) ||
			((flags & (MS_ASYNC | MS_SYNC)) == (MS_ASYNC | MS_SYNC))) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}
#endif


#ifdef WLPHX_NEED_PIPE2
int pipe2(int fds[2], int flags)
{
	int i;

	if ((flags & ~(O_CLOEXEC | O_NONBLOCK)) != 0) {
		errno = EINVAL;
		return -1;
	}
	if (pipe(fds) < 0) {
		return -1;
	}
	for (i = 0; i < 2; i++) {
		if ((((flags & O_CLOEXEC) != 0) && (fcntl(fds[i], F_SETFD, FD_CLOEXEC) < 0)) ||
				(((flags & O_NONBLOCK) != 0) && (fcntl(fds[i], F_SETFL, O_NONBLOCK) < 0))) {
			int e = errno;
			close(fds[0]);
			close(fds[1]);
			errno = e;
			return -1;
		}
	}
	return 0;
}
#endif
