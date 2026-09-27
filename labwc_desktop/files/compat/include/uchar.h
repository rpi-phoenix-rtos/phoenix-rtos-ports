/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: C11 <uchar.h> (libphoenix has none; foot uses char32_t).
 * mbrtoc32()/c32rtomb() convert UTF-8 regardless of the locale (libphoenix has
 * only the C locale; Wayland text is UTF-8): compat/src/lwphx_uchar.c. Programs
 * sizing buffers with MB_CUR_MAX (1 in libphoenix) must be built with
 * -DLWPHX_UTF8_MB_CUR_MAX (compat/include/stdlib.h), which makes it 4.
 */
#ifndef LWPHX_UCHAR_H
#define LWPHX_UCHAR_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifndef __cplusplus
typedef uint_least16_t char16_t;
typedef uint_least32_t char32_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif
size_t mbrtoc32(char32_t *pc32, const char *s, size_t n, mbstate_t *ps);
size_t c32rtomb(char *s, char32_t c32, mbstate_t *ps);
#ifdef __cplusplus
}
#endif

#endif
