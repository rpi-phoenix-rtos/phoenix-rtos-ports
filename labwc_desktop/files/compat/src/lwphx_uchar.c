/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: mbrtoc32()/c32rtomb() as UTF-8 (see compat/include/uchar.h).
 *
 * libphoenix supports only the C locale, whose multibyte layer maps each byte to
 * one wchar_t (wchar/wchar.c). The Wayland clients built here (foot) require a
 * UTF-8 locale and do all their char32_t conversions through these two
 * functions, so they decode and encode UTF-8 whatever setlocale() says --
 * Wayland text (keyboard, clipboard, titles) is UTF-8 by protocol.
 *
 * mbstate_t carries a partial sequence between calls, in its first two 32-bit
 * words: (sequence length << 8 | continuation bytes still expected), then the
 * code point bits so far. The first word is the one mbsinit() tests (libphoenix's
 * `count`, glibc's `__count`), so a pending sequence reads as a non-initial state.
 * Invalid input (a stray continuation byte, an overlong form, a surrogate,
 * > U+10FFFF) is EILSEQ and resets the state.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <uchar.h>
#include <wchar.h>

static mbstate_t lwphx_mbrtoc32_state;

_Static_assert(sizeof(mbstate_t) >= 2 * sizeof(uint32_t), "mbstate_t too small for the UTF-8 state");

static void lwphx_state_get(const mbstate_t *ps, uint32_t *head, uint32_t *cp)
{
	uint32_t w[2];

	memcpy(w, ps, sizeof(w));
	*head = w[0];
	*cp = w[1];
}


static void lwphx_state_set(mbstate_t *ps, uint32_t head, uint32_t cp)
{
	uint32_t w[2] = { head, cp };

	memset(ps, 0, sizeof(*ps));
	memcpy(ps, w, sizeof(w));
}


static uint32_t lwphx_min_for(uint32_t total)
{
	return (total == 2u) ? 0x80u : ((total == 3u) ? 0x800u : 0x10000u);
}


size_t mbrtoc32(char32_t *pc32, const char *s, size_t n, mbstate_t *ps)
{
	const unsigned char *p = (const unsigned char *)s;
	uint32_t head, cp, need, total;
	size_t i = 0;

	if (ps == NULL) {
		ps = &lwphx_mbrtoc32_state;
	}
	if (s == NULL) {
		/* mbrtoc32(NULL, "", 1, ps): reset; an incomplete sequence is an error */
		lwphx_state_get(ps, &head, &cp);
		if (head != 0u) {
			memset(ps, 0, sizeof(*ps));
			errno = EILSEQ;
			return (size_t)-1;
		}
		return 0;
	}
	if (n == 0u) {
		return (size_t)-2;
	}
	lwphx_state_get(ps, &head, &cp);
	if (head == 0u) {
		unsigned b = p[i++];
		if (b < 0x80u) {
			if (pc32 != NULL) {
				*pc32 = (char32_t)b;
			}
			return (b == 0u) ? 0u : 1u;
		}
		else if ((b & 0xe0u) == 0xc0u) {
			need = 1u;
			cp = b & 0x1fu;
		}
		else if ((b & 0xf0u) == 0xe0u) {
			need = 2u;
			cp = b & 0x0fu;
		}
		else if ((b & 0xf8u) == 0xf0u) {
			need = 3u;
			cp = b & 0x07u;
		}
		else {
			errno = EILSEQ;
			return (size_t)-1;
		}
		/* the lead byte's total length rides in the top bits of `count` */
		total = need + 1u;
	}
	else {
		need = head & 0xffu;
		total = head >> 8;
	}
	for (; (need > 0u) && (i < n); need--) {
		unsigned b = p[i++];
		if ((b & 0xc0u) != 0x80u) {
			memset(ps, 0, sizeof(*ps));
			errno = EILSEQ;
			return (size_t)-1;
		}
		cp = (cp << 6) | (b & 0x3fu);
	}
	if (need > 0u) {
		lwphx_state_set(ps, (total << 8) | need, cp);
		return (size_t)-2;
	}
	memset(ps, 0, sizeof(*ps));
	if ((cp < lwphx_min_for(total)) || (cp > 0x10ffffu) || ((cp >= 0xd800u) && (cp <= 0xdfffu))) {
		errno = EILSEQ;
		return (size_t)-1;
	}
	if (pc32 != NULL) {
		*pc32 = (char32_t)cp;
	}
	return (cp == 0u) ? 0u : i;
}


size_t c32rtomb(char *s, char32_t c32, mbstate_t *ps)
{
	unsigned char *o = (unsigned char *)s;
	unsigned c = (unsigned)c32;

	(void)ps; /* UTF-8 output is stateless */
	if (s == NULL) {
		return 1; /* as for c32rtomb(buf, U'\0', ps) */
	}
	if (c < 0x80u) {
		o[0] = (unsigned char)c;
		return 1;
	}
	if (c < 0x800u) {
		o[0] = (unsigned char)(0xc0u | (c >> 6));
		o[1] = (unsigned char)(0x80u | (c & 0x3fu));
		return 2;
	}
	if ((c >= 0xd800u) && (c <= 0xdfffu)) {
		errno = EILSEQ;
		return (size_t)-1;
	}
	if (c < 0x10000u) {
		o[0] = (unsigned char)(0xe0u | (c >> 12));
		o[1] = (unsigned char)(0x80u | ((c >> 6) & 0x3fu));
		o[2] = (unsigned char)(0x80u | (c & 0x3fu));
		return 3;
	}
	if (c <= 0x10ffffu) {
		o[0] = (unsigned char)(0xf0u | (c >> 18));
		o[1] = (unsigned char)(0x80u | ((c >> 12) & 0x3fu));
		o[2] = (unsigned char)(0x80u | ((c >> 6) & 0x3fu));
		o[3] = (unsigned char)(0x80u | (c & 0x3fu));
		return 4;
	}
	errno = EILSEQ;
	return (size_t)-1;
}
