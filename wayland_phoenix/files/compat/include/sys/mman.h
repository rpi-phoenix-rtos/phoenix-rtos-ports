/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm compat: libphoenix has no memfd_create(). compat/src/wlphx_memfd.c
 * implements it over the shmsrv server (tools/gpu-lane/weston-drm/shmsrv): the
 * descriptor names a /shm/<id> object whose pages are memExport()ed, so every
 * mmap() of it -- in any process it is passed to -- maps the same pages. Seals
 * are not supported (F_ADD_SEALS fails with EINVAL; callers ignore that).
 */
#include_next <sys/mman.h>

#ifndef WLPHX_SYS_MMAN_H
#define WLPHX_SYS_MMAN_H

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC       0x0001u
#define MFD_ALLOW_SEALING 0x0002u
#define MFD_HUGETLB       0x0004u
#define MFD_NOEXEC_SEAL   0x0008u
#define MFD_EXEC          0x0010u
#endif

/* libphoenix has no msync(): mappings of Phoenix memory objects are coherent and
 * never written back, so the stand-in only validates its arguments. */
#ifndef MS_ASYNC
#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4
#endif

#ifdef __cplusplus
extern "C" {
#endif
int memfd_create(const char *name, unsigned int flags);
/* a fresh shmsrv object id (open it as /shm/<id>); errno ENOSYS without a server */
int wlphx_shm_create(unsigned int *id);
int msync(void *addr, size_t len, int flags);
#ifdef __cplusplus
}
#endif

#endif
