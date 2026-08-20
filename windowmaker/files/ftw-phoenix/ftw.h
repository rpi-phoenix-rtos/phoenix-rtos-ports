/*
 * Phoenix-RTOS — minimal <ftw.h> for the aarch64-phoenix X11 port.
 *
 * libphoenix ships no <ftw.h> / nftw(). Window Maker's WINGs toolkit
 * (WINGs/proplist.c) uses nftw() in exactly one helper, wrmdirhier(), to
 * recursively delete a GNUstep user directory. This provides just enough of
 * the SUSv4 ftw interface for that single FTW_PHYS, depth-first call.
 *
 * Only the flags/type-codes Window Maker actually passes are meaningful;
 * the rest are declared for source compatibility.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#ifndef _FTW_H_
#define _FTW_H_

#include <sys/stat.h>

/* type codes passed to the callback */
#define FTW_F   0 /* regular file */
#define FTW_D   1 /* directory, pre-order */
#define FTW_DNR 2 /* directory that cannot be read */
#define FTW_NS  3 /* stat() failed */
#define FTW_SL  4 /* symbolic link */
#define FTW_DP  5 /* directory, post-order (nftw FTW_DEPTH) */
#define FTW_SLN 6 /* symlink to a nonexistent file */

/* nftw() flags */
#define FTW_PHYS  0x01 /* physical walk: do not follow symlinks */
#define FTW_MOUNT 0x02 /* stay within one filesystem (ignored here) */
#define FTW_CHDIR 0x04 /* chdir into each directory (ignored here) */
#define FTW_DEPTH 0x08 /* report directories post-order */

struct FTW {
	int base;  /* offset of the basename within the path */
	int level; /* depth relative to the starting path */
};

int ftw(const char *path, int (*fn)(const char *, const struct stat *, int), int nopenfd);
int nftw(const char *path, int (*fn)(const char *, const struct stat *, int, struct FTW *),
	int nopenfd, int flags);

#endif /* _FTW_H_ */
