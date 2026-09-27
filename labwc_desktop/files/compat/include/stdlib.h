/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: posix_openpt() (libphoenix has grantpt/unlockpt/ptsname but
 * not posix_openpt; compat/src/lwphx_pty.c opens /dev/ptmx) and reallocarray()
 * (fuzzel; compat/src/lwphx_misc.c).
 */
#include_next <stdlib.h>

#ifndef LWPHX_STDLIB_H
#define LWPHX_STDLIB_H

/* For programs whose char32_t conversions are compat's UTF-8 ones (uchar.h):
 * their MB_CUR_MAX-sized buffers must hold a UTF-8 sequence. */
#ifdef LWPHX_UTF8_MB_CUR_MAX
#undef MB_CUR_MAX
#define MB_CUR_MAX 4
#endif

#ifdef __cplusplus
extern "C" {
#endif
int posix_openpt(int oflag);
void *reallocarray(void *ptr, size_t nmemb, size_t size);
#ifdef __cplusplus
}
#endif

#endif
