/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: wide-string functions libphoenix lacks (foot's char32_t
 * helpers map onto them): wcsncat, wcscasecmp, wcsncasecmp. compat/src/lwphx_wchar.c.
 */
#include_next <wchar.h>

#ifndef LWPHX_WCHAR_H
#define LWPHX_WCHAR_H

#ifdef __cplusplus
extern "C" {
#endif
wchar_t *wcsncat(wchar_t *dst, const wchar_t *src, size_t n);
int wcscasecmp(const wchar_t *a, const wchar_t *b);
int wcsncasecmp(const wchar_t *a, const wchar_t *b, size_t n);
#ifdef __cplusplus
}
#endif

#endif
