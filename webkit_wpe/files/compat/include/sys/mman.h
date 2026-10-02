/*
 * Phoenix-RTOS browser track C (JavaScriptCore) -- local compat, not part of libphoenix.
 *
 * <sys/mman.h> plus madvise(), msync() and MAP_FILE. TODO(browser-B1): libphoenix has neither (browser PLAN
 * B1 adds madvise); delete them here and in ../../phoenix-jsc-compat.c when they land.
 *
 * The compat madvise() accepts the advice values below and does nothing: Phoenix cannot give
 * single pages of a mapping back to the kernel short of munmap, and every value here is a
 * hint whose no-op is correct as long as no caller relies on Linux's "MADV_DONTNEED zero-fills"
 * (WebKit does only under OS(LINUX)). bmalloc/WTF call it in `while (madvise() == -1 && errno ==
 * EAGAIN)` loops; mimalloc on Phoenix does not call it at all (its Phoenix prim).
 *
 * msync() fails with ENOSYS: Phoenix has no shared file mappings (MAP_SHARED == MAP_PRIVATE,
 * KNOWN-ISSUES P21), so there is never anything to write back. WTF's FileSystem::mapToFile()
 * (JSC bytecode disk cache, WebKit's network cache) therefore cannot work on Phoenix as written;
 * the jsc shell does not use it.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PHOENIX_JSC_COMPAT_SYS_MMAN_H
#define PHOENIX_JSC_COMPAT_SYS_MMAN_H

#include_next <sys/mman.h>

/* MAP_FILE is the historical "not anonymous" flag, 0 on Linux and the BSDs. */
#ifndef MAP_FILE
#define MAP_FILE 0
#endif

#ifndef MADV_NORMAL
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4

#ifdef __cplusplus
extern "C" {
#endif

int madvise(void *addr, size_t len, int advice);

#ifdef __cplusplus
}
#endif
#endif

#ifndef MS_ASYNC
#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4

#ifdef __cplusplus
extern "C" {
#endif

int msync(void *addr, size_t len, int flags);

#ifdef __cplusplus
}
#endif
#endif

#endif
