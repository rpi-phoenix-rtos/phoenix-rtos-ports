/*
 * Phoenix-RTOS — minimal <langinfo.h> for Midnight Commander (and other
 * locale-charset-aware ports). Phoenix libc has no langinfo.
 *
 * Provides nl_item + the common nl_langinfo item codes and the nl_langinfo()
 * prototype. The implementation (langinfo-stub.c) returns fixed C/POSIX-locale
 * strings; nl_langinfo(CODESET) returns "UTF-8" to match the identity stub
 * libiconv (mc then treats the terminal as UTF-8, correct for a UTF-8 console).
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#ifndef _PHOENIX_LANGINFO_H
#define _PHOENIX_LANGINFO_H

#include <locale.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int nl_item;

/* Item codes. CODESET is what mc/glib actually query; the rest let other ports
 * compile. Values are arbitrary-but-stable small integers. */
#define CODESET       0
#define D_T_FMT       1
#define D_FMT         2
#define T_FMT         3
#define T_FMT_AMPM    4
#define AM_STR        5
#define PM_STR        6
#define DAY_1         7
#define ABDAY_1       8
#define MON_1         9
#define ABMON_1       10
#define RADIXCHAR     11
#define THOUSEP       12
#define YESEXPR       13
#define NOEXPR        14
#define CRNCYSTR      15

char *nl_langinfo(nl_item item);

#ifdef __cplusplus
}
#endif

#endif /* _PHOENIX_LANGINFO_H */
