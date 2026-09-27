/*
 * Phoenix-RTOS
 *
 * mesa-drm compat (new GPU lane, M3 part 3) - stand-ins for libphoenix gaps
 *
 * Each function is compiled only while libphoenix lacks the symbol (build.sh
 * passes the MESADRM_NEED_* defines after checking libphoenix.a), so this file
 * never duplicates a real implementation. Delete it when libphoenix branch
 * gpu-lane/libc-gaps (open_memstream: 7cc5628), getopt_long_only() and sincos() land.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <getopt.h>
#include <math.h>


#ifdef MESADRM_NEED_OPEN_MEMSTREAM
/* Mesa (util/memstream.c) and libdrm use memory streams only to format debug
 * text (NIR/SPIR-V dumps, modifier names); every caller handles NULL. */
FILE *open_memstream(char **bufp, size_t *sizep)
{
	(void)bufp;
	(void)sizep;
	errno = ENOSYS;
	return NULL;
}
#endif


#ifdef MESADRM_NEED_GETOPT_LONG_ONLY
/* Differs from glibc only for single-dash long options ("-count=10"), which are
 * parsed as short-option clusters here. */
int getopt_long_only(int argc, char *const argv[], const char *optstring,
	const struct option *longopts, int *longindex)
{
	return getopt_long(argc, argv, optstring, longopts, longindex);
}
#endif


#ifdef MESADRM_NEED_SINCOS
/* GNU extension (kmscube cube-gears.c). */
void sincos(double x, double *s, double *c)
{
	*s = sin(x);
	*c = cos(x);
}
#endif
