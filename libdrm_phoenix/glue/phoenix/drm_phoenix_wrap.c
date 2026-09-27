/*
 * Phoenix-RTOS
 *
 * libdrm-phoenix - optional mmap() interposer for unmodified DRM programs
 *
 * Linux DRM programs map buffers with mmap(drm_fd, offset) where offset came
 * from MMAP_BO / MAP_DUMB. On Phoenix a node descriptor's object is not the
 * buffer (the server's per-open id is its oid, and render BOs are separate
 * blocks), so that call must become drmPhoenixMmap(). Two ways:
 *   - patch the call site (Mesa: 4 sites, see docs/gpu-new-lane/M3-libdrm-phoenix.md);
 *   - link the program with -Wl,--wrap=mmap: every mmap() call in the static
 *     binary then lands here, DRM tokens are resolved, everything else goes to
 *     the real mmap() unchanged.
 * This object is its own archive member: it is pulled into a link only by
 * --wrap=mmap (it references __real_mmap, which exists only then).
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <sys/mman.h>

#include "libdrm_macros.h"
#include "xf86drm.h"
#include "drm_phoenix_priv.h"


extern void *__real_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
drm_public void *__wrap_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);


drm_public void *__wrap_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	kms_memref_t m;

	/* Anonymous and physical mappings (malloc, MAP_PHYSMEM) never reach the DRM
	 * tables. The token path's own mmap() calls re-enter here with fd < 0 (PHYS)
	 * or a buffer-name descriptor at offset 0, both of which end in __real_mmap -
	 * never back in drmPhoenixMmap, so there is no recursion. */
	if (fd < 0) {
		return __real_mmap(addr, length, prot, flags, fd, offset);
	}
	if (DRMPHX_TOKEN_OK(offset)) {
		return drmPhoenixMmap(addr, length, prot, flags, fd, offset);
	}
	if ((offset == 0) && (drmphx_prime_fd_lookup(fd, &m) == 0)) {
		/* a dma-buf descriptor: the export's memory type is mandatory (E1) */
		void *p = __real_mmap(addr, length, prot,
			(flags & MAP_FIXED) | ((m.cache == KMS_CACHE_UNCACHED) ? MAP_UNCACHED : 0), fd, 0);
		if (drmphx_trace_enabled() != 0) {
			drmphx_trace_mmap("dmabuf", fd, 0, length, p);
		}
		return p;
	}
	return __real_mmap(addr, length, prot, flags, fd, offset);
}
