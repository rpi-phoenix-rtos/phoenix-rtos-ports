/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: posix_openpt() (foot). posixsrv serves /dev/ptmx; libphoenix
 * already has grantpt(), unlockpt() and ptsname().
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>

int posix_openpt(int oflag)
{
	if ((oflag & ~(O_RDWR | O_NOCTTY | O_CLOEXEC)) != 0) {
		errno = EINVAL;
		return -1;
	}
	return open("/dev/ptmx", oflag);
}
