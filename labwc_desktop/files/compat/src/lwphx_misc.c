/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: small libphoenix gaps.
 */

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

/* realloc() of nmemb * size, refusing a product that overflows (OpenBSD/glibc). */
void *reallocarray(void *ptr, size_t nmemb, size_t size)
{
	if ((size != 0u) && (nmemb > SIZE_MAX / size)) {
		errno = ENOMEM;
		return NULL;
	}
	return realloc(ptr, nmemb * size);
}


/* libphoenix's DIR (<stdio.h>, struct _DIR) holds a descriptor only after fdopendir(). */
int dirfd(DIR *dirp)
{
	if ((dirp == NULL) || (dirp->fd < 0)) {
		errno = (dirp == NULL) ? EINVAL : ENOTSUP;
		return -1;
	}
	return dirp->fd;
}
