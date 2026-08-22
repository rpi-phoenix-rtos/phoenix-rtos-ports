/*
 * Phoenix-RTOS — xbill build shim (force-included by build-xbill.sh).
 *
 * xbill ships its own "strings.h" (a table of game UI strings) in the source
 * dir, and the build adds that dir to the include path with -I. — so the
 * SYSTEM <strings.h> is shadowed and the prototypes for strncasecmp()/
 * strcasecmp() never get declared, even though libc provides both symbols.
 * Re-declare them here (matching the libc signatures) so Game.c/UI.c compile
 * without an implicit-declaration error. This is purely a header-shadowing
 * workaround; no behaviour change.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#ifndef XBILL_PHOENIX_SHIM_H
#define XBILL_PHOENIX_SHIM_H

#include <stddef.h> /* size_t */

extern int strcasecmp(const char *s1, const char *s2);
extern int strncasecmp(const char *s1, const char *s2, size_t n);

#endif /* XBILL_PHOENIX_SHIM_H */
