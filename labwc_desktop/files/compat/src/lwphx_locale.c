/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: locale objects for a libc with only the C locale (see
 * compat/include/locale.h). newlocale() accepts "C", "POSIX" and "" (which are
 * all the C locale here) and returns one static object; uselocale() records
 * the thread-independent current object and returns the previous one.
 */

#include <errno.h>
#include <locale.h>
#include <string.h>

struct lwphx_locale {
	int unused;
};

static struct lwphx_locale lwphx_c_locale;
static locale_t lwphx_current = LC_GLOBAL_LOCALE;


locale_t newlocale(int category_mask, const char *locale, locale_t base)
{
	(void)base;
	if (((category_mask & ~LC_ALL_MASK) != 0) || (locale == NULL)) {
		errno = EINVAL;
		return (locale_t)0;
	}
	if ((strcmp(locale, "C") != 0) && (strcmp(locale, "POSIX") != 0) && (locale[0] != '\0')) {
		errno = ENOENT;
		return (locale_t)0;
	}
	return &lwphx_c_locale;
}


locale_t uselocale(locale_t newloc)
{
	locale_t old = lwphx_current;

	if (newloc != (locale_t)0) {
		lwphx_current = newloc;
	}
	return old;
}


void freelocale(locale_t loc)
{
	(void)loc;
}


locale_t duplocale(locale_t loc)
{
	(void)loc;
	return &lwphx_c_locale;
}
