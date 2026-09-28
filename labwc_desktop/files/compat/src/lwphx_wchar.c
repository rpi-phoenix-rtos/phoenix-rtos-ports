/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: wcsncat, wcscasecmp, wcsncasecmp (see compat/include/wchar.h).
 * Case folding is towlower() (libphoenix: ASCII only in the C locale).
 */

#include <wchar.h>
#include <wctype.h>

wchar_t *wcsncat(wchar_t *dst, const wchar_t *src, size_t n)
{
	wchar_t *d = dst;

	while (*d != L'\0') {
		d++;
	}
	while ((n > 0u) && (*src != L'\0')) {
		*d++ = *src++;
		n--;
	}
	*d = L'\0';
	return dst;
}


int wcsncasecmp(const wchar_t *a, const wchar_t *b, size_t n)
{
	wint_t ca, cb;

	for (; n > 0u; n--, a++, b++) {
		ca = towlower((wint_t)*a);
		cb = towlower((wint_t)*b);
		if (ca != cb) {
			return (ca < cb) ? -1 : 1;
		}
		if (ca == L'\0') {
			break;
		}
	}
	return 0;
}


int wcscasecmp(const wchar_t *a, const wchar_t *b)
{
	return wcsncasecmp(a, b, (size_t)-1);
}
