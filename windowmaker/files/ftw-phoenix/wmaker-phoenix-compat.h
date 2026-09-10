/*
 * Phoenix-RTOS X11 port — prototypes for libc gap-fills carried in libftw.a
 * (see ftw.c in this directory). This header is installed into the wmaker
 * dependency prefix and -include'd into the Window Maker build so the call
 * sites have a declaration; the definitions live in libftw.a.
 *
 * These cover symbols libphoenix does not provide that Window Maker (and its
 * util/ helpers) reference. Each is listed as a known libphoenix gap in
 * ../WMAKER-PORT-STATUS.md.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#ifndef _WMAKER_PHOENIX_COMPAT_H_
#define _WMAKER_PHOENIX_COMPAT_H_

#include <dirent.h>

/* nice() is declared by libphoenix <unistd.h> now (and implemented there as the
 * same SCHED_RR no-op), so neither the declaration nor the definition belongs
 * here -- see the note in ftw.c, where keeping the definition was a build break.
 */

/* directory listing helpers — POSIX, absent from libphoenix <dirent.h> */
int alphasort(const struct dirent **a, const struct dirent **b);
int scandir(const char *dirp, struct dirent ***namelist,
	int (*filter)(const struct dirent *),
	int (*compar)(const struct dirent **, const struct dirent **));

#endif /* _WMAKER_PHOENIX_COMPAT_H_ */
