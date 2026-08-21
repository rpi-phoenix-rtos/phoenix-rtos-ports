/*
 * Phoenix-RTOS — minimal <libintl.h> stub for cross-building glib2 (and other
 * NLS-aware autotools packages) without porting full GNU gettext.
 *
 * Phoenix libc has no gettext. glib-2.56 HARD-requires a gettext provider even
 * with --disable-nls (configure errors "You must have gettext support..."). This
 * header satisfies both the configure header probe and compilation by defining
 * the gettext family as identity macros: gettext(msgid) -> msgid. With NLS
 * disabled there are no catalogs anyway, so identity is the correct behaviour.
 *
 * No runtime symbols are emitted (all macros), so nothing extra is needed at
 * link time. Replace with real gettext only if localized message catalogs are
 * ever wanted on Phoenix.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#ifndef _PHOENIX_LIBINTL_H
#define _PHOENIX_LIBINTL_H

#include <locale.h>

#ifdef __cplusplus
extern "C" {
#endif

#define gettext(Msgid)                              ((char *)(Msgid))
#define dgettext(Domainname, Msgid)                 ((char *)(Msgid))
#define dcgettext(Domainname, Msgid, Category)      ((char *)(Msgid))
#define ngettext(Msgid1, Msgid2, N)                 ((char *)((N) == 1 ? (Msgid1) : (Msgid2)))
#define dngettext(Dom, Msgid1, Msgid2, N)           ((char *)((N) == 1 ? (Msgid1) : (Msgid2)))
#define dcngettext(Dom, Msgid1, Msgid2, N, Cat)     ((char *)((N) == 1 ? (Msgid1) : (Msgid2)))
#define textdomain(Domainname)                      ((char *)(Domainname))
#define bindtextdomain(Domainname, Dirname)         ((char *)(Dirname))
#define bind_textdomain_codeset(Domainname, Codeset) ((char *)(Codeset))

#ifdef __cplusplus
}
#endif

#endif /* _PHOENIX_LIBINTL_H */
