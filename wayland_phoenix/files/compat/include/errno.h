/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm compat: glibc's program_invocation_short_name (declared in <errno.h>
 * under _GNU_SOURCE; weston's xalloc.h prints it on out-of-memory). libphoenix
 * has the BSD getprogname() instead.
 */
#include_next <errno.h>

#ifndef program_invocation_short_name
#ifdef __cplusplus
extern "C"
#endif
const char *getprogname(void);
#define program_invocation_short_name ((char *)getprogname())
#endif
