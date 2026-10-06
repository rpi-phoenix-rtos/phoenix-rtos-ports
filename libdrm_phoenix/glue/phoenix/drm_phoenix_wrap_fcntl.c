/*
 * Phoenix-RTOS
 *
 * libdrm-phoenix - optional fcntl() interposer: a sync file duplicated with
 * F_DUPFD / F_DUPFD_CLOEXEC is still a sync file
 *
 * An emulated sync file is a dup() of the render node descriptor plus a fence
 * set in a process table keyed by descriptor number (M3 section 2.8). The kernel
 * cannot tell a duplicate of one sync file from a duplicate of another (all of
 * them share the render node's open file), so a copy the program makes itself is
 * known to this library only if it sees the copy being made. Mesa makes such a
 * copy on every EGL native-fence path: util/libsync.h sync_accumulate() and
 * gallium v3d's create_fence_fd/get_fence_fd use fcntl(fd, F_DUPFD_CLOEXEC, 3)
 * (os_dupfd_cloexec), so without this interposer every GPU-side wait on an EGL
 * native fence (eglWaitSyncKHR) failed at the next submit with "Failed to import
 * native fence." and the job ran without its in-fence.
 *
 * Linked with -Wl,--wrap=fcntl, every fcntl() of the static binary lands here;
 * the request goes to the real fcntl() unchanged, and a successful F_DUPFD /
 * F_DUPFD_CLOEXEC of a sync file registers the new descriptor with the same
 * fences. Its own archive member (it references __real_fcntl, which exists only
 * under --wrap=fcntl). The dup()/dup2() half is drm_phoenix_wrap_dup.c.
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
#include <stdarg.h>

#include "libdrm_macros.h"
#include "xf86drm.h"
#include "drm_phoenix_priv.h"


extern int __real_fcntl(int fd, int cmd, ...);
drm_public int __wrap_fcntl(int fd, int cmd, ...);


drm_public int __wrap_fcntl(int fd, int cmd, ...)
{
	va_list ap;
	unsigned long arg;
	int rc, err;

	/* The third argument is an int or a pointer (struct flock *); read it at full
	 * register width and pass it on as such, as libphoenix's fcntl() reads it. */
	va_start(ap, cmd);
	arg = va_arg(ap, unsigned long);
	va_end(ap);

	rc = __real_fcntl(fd, cmd, arg);
	if ((rc >= 0) && ((cmd == F_DUPFD) || (cmd == F_DUPFD_CLOEXEC))) {
		err = errno;
		drmphx_note_dup(fd, rc, "fcntl");
		errno = err;
	}
	return rc;
}
