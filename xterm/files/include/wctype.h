/*
 * Phoenix-RTOS — minimal <wctype.h> for building xterm.
 *
 * libphoenix has no <wctype.h>. xterm includes it from three translation
 * units but only three wide-character classifiers are actually compiled
 * (the wctype()/iswctype() uses are inside #ifdef TEST_DRIVER, never built):
 *   - charclass.c: iswspace(), iswalnum()  (word-selection class boundaries)
 *   - misc.c:      iswcntrl()              (control-char detection)
 * We provide those plus the rest of the standard isw and tow classifier
 * family as small static-inline functions so the existing wctype.h includes
 * and call sites compile unchanged.
 *
 * Semantics: for code points below 128 we delegate to the narrow <ctype.h>
 * classifiers (exact for ASCII); for >= 128 we return reasonable defaults
 * (treat as printable/alnum, only the C1 range 0x80-0x9F as control). This is
 * sufficient for an ASCII-keyboard terminal demo. A full, locale-correct
 * <wctype.h> in libphoenix is the upstream follow-up (tracked with task #12).
 *
 * This header lives on an xterm-only -I path; it does NOT touch the shared X11
 * prefix or libphoenix, so it cannot regress the other X clients.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#ifndef XTERM_PHOENIX_WCTYPE_H
#define XTERM_PHOENIX_WCTYPE_H

#include <ctype.h>
#include <wchar.h> /* wint_t, WEOF */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

typedef wint_t wctype_t;

static inline int iswascii(wint_t wc) { return wc < 128; }

static inline int iswspace(wint_t wc) { return (wc < 128) ? isspace((int)wc) : 0; }
static inline int iswblank(wint_t wc) { return (wc == L' ' || wc == L'\t'); }
static inline int iswcntrl(wint_t wc)
{
	return (wc < 128) ? iscntrl((int)wc) : ((wc >= 0x80 && wc <= 0x9f) ? 1 : 0);
}
static inline int iswdigit(wint_t wc) { return (wc < 128) ? isdigit((int)wc) : 0; }
static inline int iswxdigit(wint_t wc) { return (wc < 128) ? isxdigit((int)wc) : 0; }
static inline int iswlower(wint_t wc) { return (wc < 128) ? islower((int)wc) : 0; }
static inline int iswupper(wint_t wc) { return (wc < 128) ? isupper((int)wc) : 0; }
static inline int iswalpha(wint_t wc) { return (wc < 128) ? isalpha((int)wc) : 1; }
static inline int iswalnum(wint_t wc) { return (wc < 128) ? isalnum((int)wc) : 1; }
static inline int iswpunct(wint_t wc) { return (wc < 128) ? ispunct((int)wc) : 0; }
static inline int iswgraph(wint_t wc)
{
	return (wc < 128) ? isgraph((int)wc) : (iswcntrl(wc) ? 0 : 1);
}
static inline int iswprint(wint_t wc)
{
	return (wc < 128) ? isprint((int)wc) : (iswcntrl(wc) ? 0 : 1);
}

static inline wint_t towlower(wint_t wc) { return (wc < 128) ? (wint_t)tolower((int)wc) : wc; }
static inline wint_t towupper(wint_t wc) { return (wc < 128) ? (wint_t)toupper((int)wc) : wc; }

#ifdef __cplusplus
}
#endif

#endif /* XTERM_PHOENIX_WCTYPE_H */
