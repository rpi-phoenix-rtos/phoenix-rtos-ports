/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: POSIX shared memory objects (libphoenix has no shm_open()).
 * compat/src/lwphx_shm.c keeps a per-process name table over shmsrv objects
 * (the memfd_create() backing of weston-drm's compat, next on the include path).
 * Names are visible only inside the process that created them -- wlroots' only
 * pattern is create, open a second (read-only) descriptor, unlink at once.
 */
#include_next <sys/mman.h>

#ifndef LWPHX_SYS_MMAN_H
#define LWPHX_SYS_MMAN_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif
int shm_open(const char *name, int oflag, mode_t mode);
int shm_unlink(const char *name);
#ifdef __cplusplus
}
#endif

#endif
