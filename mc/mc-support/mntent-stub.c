/*
 * Phoenix-RTOS — stub mntent API for Midnight Commander.
 *
 * Phoenix libc has no getmntent/setmntent/endmntent (no /etc/mtab mount-table
 * enumeration). mc's mountlist.c uses them to populate the filesystem free-space
 * panel. This stub reports NO mounted filesystems (setmntent returns a dummy
 * non-NULL handle, getmntent always returns NULL = end-of-list). mc then shows an
 * empty mount list, which is acceptable for a first console MC on Phoenix.
 *
 * Replace with a real mount-table source (e.g. reading Phoenix's mount info) for
 * a populated free-space panel.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#include <stdio.h>
#include <mntent.h>

FILE *setmntent(const char *filename, const char *type)
{
	(void)filename;
	(void)type;
	/* Return a non-NULL sentinel; mc only passes it back to get/endmntent. */
	return (FILE *)1;
}

struct mntent *getmntent(FILE *stream)
{
	(void)stream;
	return NULL; /* no entries */
}

int endmntent(FILE *stream)
{
	(void)stream;
	return 1;
}
