/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: POSIX.1-2008 locale objects (newlocale/uselocale/freelocale;
 * fcft switches LC_NUMERIC to "C" around fontconfig's number parsing). libphoenix
 * has only the C locale, so every locale object is it: compat/src/lwphx_locale.c.
 */
#include_next <locale.h>

#ifndef LWPHX_LOCALE_H
#define LWPHX_LOCALE_H

/* libphoenix has no LC_MESSAGES (fuzzel sets it): an unknown category to its
 * setlocale(), which answers NULL -- messages stay untranslated either way. */
#ifndef LC_MESSAGES
#define LC_MESSAGES 6
#endif

#ifndef LC_GLOBAL_LOCALE
typedef struct lwphx_locale *locale_t;

#define LC_CTYPE_MASK    (1 << LC_CTYPE)
#define LC_NUMERIC_MASK  (1 << LC_NUMERIC)
#define LC_TIME_MASK     (1 << LC_TIME)
#define LC_COLLATE_MASK  (1 << LC_COLLATE)
#define LC_MONETARY_MASK (1 << LC_MONETARY)
#define LC_ALL_MASK      (LC_CTYPE_MASK | LC_NUMERIC_MASK | LC_TIME_MASK | LC_COLLATE_MASK | LC_MONETARY_MASK)
#define LC_GLOBAL_LOCALE ((locale_t)-1)

#ifdef __cplusplus
extern "C" {
#endif
locale_t newlocale(int category_mask, const char *locale, locale_t base);
locale_t uselocale(locale_t newloc);
void freelocale(locale_t loc);
locale_t duplocale(locale_t loc);
#ifdef __cplusplus
}
#endif
#endif

#endif
