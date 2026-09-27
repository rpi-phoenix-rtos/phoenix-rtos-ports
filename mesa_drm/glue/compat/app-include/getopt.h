/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * mesa-drm compat: libphoenix gap -- getopt_long_only() is missing (kmscube uses
 * it). compat/mesadrm_compat.c maps it to getopt_long() while libphoenix lacks it,
 * so single-dash LONG options ("-count=10") are not recognised; short options and
 * "--long" options behave exactly as with glibc.
 */
#include_next <getopt.h>
#ifndef MESADRM_COMPAT_GETOPT_LONG_ONLY
#define MESADRM_COMPAT_GETOPT_LONG_ONLY
#ifdef __cplusplus
extern "C"
#endif
int getopt_long_only(int argc, char *const argv[], const char *optstring,
	const struct option *longopts, int *longindex);
#endif
