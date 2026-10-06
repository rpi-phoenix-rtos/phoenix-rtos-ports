/*
 * Phoenix-RTOS
 *
 * libdrm-phoenix - optional dup()/dup2() interposer: a duplicated sync file is
 * still a sync file
 *
 * The dup()/dup2() half of drm_phoenix_wrap_fcntl.c (see there for why): linked
 * with -Wl,--wrap=dup -Wl,--wrap=dup2, a successful duplicate of one of this
 * process's emulated sync files is registered with the same fences, and a stale
 * table entry on the new descriptor number is dropped. Its own archive member
 * (it references __real_dup and __real_dup2, which exist only under both flags).
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <unistd.h>

#include "libdrm_macros.h"
#include "xf86drm.h"
#include "drm_phoenix_priv.h"


extern int __real_dup(int fd);
extern int __real_dup2(int fd, int fd2);
drm_public int __wrap_dup(int fd);
drm_public int __wrap_dup2(int fd, int fd2);


drm_public int __wrap_dup(int fd)
{
	int rc = __real_dup(fd), err;

	if (rc >= 0) {
		err = errno;
		drmphx_note_dup(fd, rc, "dup");
		errno = err;
	}
	return rc;
}


drm_public int __wrap_dup2(int fd, int fd2)
{
	int rc = __real_dup2(fd, fd2), err;

	if ((rc >= 0) && (rc != fd)) {   /* dup2(fd, fd) changes nothing */
		err = errno;
		drmphx_note_dup(fd, rc, "dup2");
		errno = err;
	}
	return rc;
}
