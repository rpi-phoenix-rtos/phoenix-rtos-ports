/*
 * Phoenix-RTOS — minimal nftw()/ftw() for the aarch64-phoenix X11 port.
 *
 * libphoenix has no nftw(). This is a small recursive implementation covering
 * the subset Window Maker's WINGs toolkit needs (FTW_PHYS, depth-first, used
 * by wrmdirhier() to remove a GNUstep user directory tree). It is NOT a
 * general-purpose fts-grade walker:
 *   - FTW_MOUNT and FTW_CHDIR are accepted but ignored.
 *   - The nopenfd descriptor budget is ignored (recursion uses the C stack).
 *   - On a non-zero callback return, the walk stops and returns that value,
 *     matching SUSv4.
 *
 * Type codes reported: FTW_F, FTW_D (pre-order), FTW_DP (post-order, when
 * FTW_DEPTH is set), FTW_SL (symlink, when FTW_PHYS is set), FTW_NS (stat
 * failed), FTW_DNR (directory unreadable).
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#include <ftw.h>

#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

/* declarations for the libc gap-fills also defined in this TU */
#include "wmaker-phoenix-compat.h"

static int do_nftw(char *path, size_t pathlen,
	int (*fn)(const char *, const struct stat *, int, struct FTW *),
	int flags, int level)
{
	struct stat st;
	struct FTW ftw;
	int rc;
	const char *base;

	/* base = offset of the last path component */
	base = strrchr(path, '/');
	ftw.base = (base != NULL) ? (int)(base - path) + 1 : 0;
	ftw.level = level;

	/* FTW_PHYS => lstat (do not follow symlinks); else stat. Phoenix may not
	 * provide lstat; fall back to stat when it is unavailable at link time. */
	if ((flags & FTW_PHYS) != 0) {
		if (lstat(path, &st) != 0) {
			return fn(path, &st, FTW_NS, &ftw);
		}
	}
	else {
		if (stat(path, &st) != 0) {
			return fn(path, &st, FTW_NS, &ftw);
		}
	}

	if (S_ISLNK(st.st_mode) && (flags & FTW_PHYS) != 0) {
		return fn(path, &st, FTW_SL, &ftw);
	}

	if (!S_ISDIR(st.st_mode)) {
		return fn(path, &st, FTW_F, &ftw);
	}

	/* Directory: pre-order report unless FTW_DEPTH. */
	if ((flags & FTW_DEPTH) == 0) {
		rc = fn(path, &st, FTW_D, &ftw);
		if (rc != 0) {
			return rc;
		}
	}

	{
		DIR *dir = opendir(path);
		struct dirent *de;

		if (dir == NULL) {
			return fn(path, &st, FTW_DNR, &ftw);
		}

		while ((de = readdir(dir)) != NULL) {
			size_t namelen, newlen;
			char *child;

			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
				continue;
			}

			namelen = strlen(de->d_name);
			newlen = pathlen + 1 + namelen;
			child = malloc(newlen + 1);
			if (child == NULL) {
				closedir(dir);
				return -1;
			}
			memcpy(child, path, pathlen);
			child[pathlen] = '/';
			memcpy(child + pathlen + 1, de->d_name, namelen);
			child[newlen] = '\0';

			rc = do_nftw(child, newlen, fn, flags, level + 1);
			free(child);
			if (rc != 0) {
				closedir(dir);
				return rc;
			}
		}
		closedir(dir);
	}

	/* Directory post-order report when FTW_DEPTH. */
	if ((flags & FTW_DEPTH) != 0) {
		/* re-fetch base/level (unchanged) for the post-order callback */
		ftw.level = level;
		return fn(path, &st, FTW_DP, &ftw);
	}

	return 0;
}

int nftw(const char *path, int (*fn)(const char *, const struct stat *, int, struct FTW *),
	int nopenfd, int flags)
{
	char *buf;
	size_t len;
	int rc;

	(void)nopenfd;

	if (path == NULL || fn == NULL) {
		return -1;
	}

	len = strlen(path);
	/* strip a single trailing slash (except for the root "/") */
	while (len > 1 && path[len - 1] == '/') {
		len--;
	}

	buf = malloc(len + 1);
	if (buf == NULL) {
		return -1;
	}
	memcpy(buf, path, len);
	buf[len] = '\0';

	rc = do_nftw(buf, len, fn, flags, 0);
	free(buf);
	return rc;
}

/* ftw(): same walk, classic 3-arg callback, no FTW struct, no FTW_PHYS. */
struct ftw_shim {
	int (*fn)(const char *, const struct stat *, int);
};

static struct ftw_shim ftw_ctx;

static int ftw_trampoline(const char *p, const struct stat *s, int t, struct FTW *w)
{
	(void)w;
	return ftw_ctx.fn(p, s, t);
}

int ftw(const char *path, int (*fn)(const char *, const struct stat *, int), int nopenfd)
{
	ftw_ctx.fn = fn;
	return nftw(path, ftw_trampoline, nopenfd, 0);
}

/*
 * nice() was shimmed here as a no-op because libphoenix had no process-priority
 * API. It has one now -- libphoenix 0961476, "unistd: implement nice as a noop
 * for SCHED_RR", which is the same no-op for the same reason (the kernel runs
 * only SCHED_RR, and POSIX says SCHED_RR threads are unaffected by nice) -- and
 * <unistd.h> declares it. Keeping ours turned a redundant definition into a
 * BUILD BREAK, since both land in the static link:
 *
 *   ld: libm.a(sys.o): in function `nice':
 *       multiple definition of `nice';
 *       libftw.a(ftw.o): first defined here
 *
 * Latent until something forced a wmaker relink, which is why the shipped
 * binary predates it: the whole showcase was one rebuild away from failing to
 * build. Before adding a shim here, check whether libphoenix has grown the real
 * function -- ftw/nftw below were re-checked and are still genuinely absent, so
 * they stay. scandir()/alphasort() went the same way as nice(): libphoenix has had
 * them since 2026-09-27 (merge 8fb82ae), so the local copies were removed.
 */
