/*
 * Phoenix-RTOS — stub nl_langinfo for Midnight Commander.
 *
 * Phoenix libc has no langinfo. Returns fixed C/POSIX-locale strings.
 *
 * nl_langinfo(CODESET) drives mc's str_init_strings codeset selection
 * (lib/strutil/strutil.c str_choose_str_functions):
 *   "UTF-8" -> str_utf8_init() (strutilutf8.c, g_utf8_* multibyte path)
 *   "ASCII" -> neither utf8 nor 8bit table matches -> str_ascii_init()
 *              (strutilascii.c, plain 8-bit path; == mc's DEFAULT_CHARSET)
 * Default is "UTF-8" (matches the identity stub libiconv). Define
 * MC_CODESET_ASCII at compile time to force the ASCII path instead (build-mc.sh
 * MC_VARIANT=ascii) — used to sidestep the UTF-8 strutil path during bring-up.
 *
 * Replace with a real locale-backed implementation for full localized
 * date/number formatting.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#include "langinfo.h"

#ifdef MC_CODESET_ASCII
#define MC_CODESET_STR "ASCII"
#else
#define MC_CODESET_STR "UTF-8"
#endif

char *nl_langinfo(nl_item item)
{
	switch (item) {
	case CODESET:    return (char *)MC_CODESET_STR;
	case D_T_FMT:    return (char *)"%a %b %e %H:%M:%S %Y";
	case D_FMT:      return (char *)"%m/%d/%y";
	case T_FMT:      return (char *)"%H:%M:%S";
	case T_FMT_AMPM: return (char *)"%I:%M:%S %p";
	case AM_STR:     return (char *)"AM";
	case PM_STR:     return (char *)"PM";
	case RADIXCHAR:  return (char *)".";
	case THOUSEP:    return (char *)"";
	case YESEXPR:    return (char *)"^[yY]";
	case NOEXPR:     return (char *)"^[nN]";
	case CRNCYSTR:   return (char *)"";
	default:         return (char *)"";
	}
}
