/*
 * Phoenix-RTOS browser track C (JavaScriptCore) -- local compat, not part of libphoenix.
 *
 * <locale.h> plus LC_MESSAGES. TODO(browser-B1): libphoenix defines LC_ALL..LC_TIME only (POSIX
 * requires LC_MESSAGES too). Its setlocale() returns NULL for a category it does not know, which
 * WTF (text/unix/TextBreakIteratorInternalICUUnix.cpp) already treats as "no locale set".
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PHOENIX_JSC_COMPAT_LOCALE_H
#define PHOENIX_JSC_COMPAT_LOCALE_H

#include_next <locale.h>

#ifndef LC_MESSAGES
#define LC_MESSAGES 6
#endif

#endif
