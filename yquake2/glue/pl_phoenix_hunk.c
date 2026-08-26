/*
 * Copyright (C) 1997-2001 Id Software, Inc.
 * Copyright (C) 2026 Phoenix Systems. Author: Witold Bołt.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * =======================================================================
 *
 * Phoenix-RTOS Hunk_* backend (replaces backends/unix/shared/hunk.c).
 *
 * The unix hunk reserves a large anonymous mmap and shrinks it with
 * mremap()/munmap() in Hunk_End(). Phoenix has no mremap and its
 * MAP_ANONYMOUS semantics for large lazily-committed reservations are not
 * relied upon here, so this is a straightforward malloc-backed hunk: the
 * full maxsize is committed up front (over-reservation, no shrink) — the
 * same strategy Quake1's DEFAULT_MEMORY hunk uses. Fine for the demo/
 * timedemo workload; a Phase-2 memory note if peak RSS matters.
 *
 * =======================================================================
 */

#include <stdlib.h>

#include "common/header/common.h"

byte *membase;
size_t maxhunksize;
size_t curhunksize;

void *
Hunk_Begin(int maxsize)
{
	/* plus sizeof(size_t) for the stored size + 32 bytes cacheline slack */
	maxhunksize = maxsize + sizeof(size_t) + 32;
	curhunksize = 0;

	membase = (byte *)malloc(maxhunksize);

	if (membase == NULL)
	{
		Sys_Error("unable to allocate %d bytes", maxsize);
	}

	*((size_t *)membase) = curhunksize;

	return membase + sizeof(size_t);
}

void *
Hunk_Alloc(int size)
{
	byte *buf;

	/* round to cacheline */
	size = (size + 31) & ~31;

	if (curhunksize + size > maxhunksize)
	{
		Sys_Error("%s: overflow %d > %d",
			__func__, (int)(curhunksize + size), (int)maxhunksize);
	}

	buf = membase + sizeof(size_t) + curhunksize;
	curhunksize += size;
	return buf;
}

int
Hunk_End(void)
{
	/* No shrink: the malloc block stays at maxhunksize. Just record the
	 * used size the way the mmap backend does. */
	*((size_t *)membase) = curhunksize + sizeof(size_t);

	return curhunksize;
}

void
Hunk_Free(void *base)
{
	if (base)
	{
		byte *m = ((byte *)base) - sizeof(size_t);
		free(m);
	}
}
