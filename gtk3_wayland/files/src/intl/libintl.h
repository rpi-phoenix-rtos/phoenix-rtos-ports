/*
 * gtk3-wayland: <libintl.h> for a build without NLS (Phoenix-RTOS has no gettext).
 *
 * GLib is built with -Dnls=disabled, but <glib/gi18n.h> and <glib/gi18n-lib.h>
 * include <libintl.h> unconditionally, and GLib's meson requires an intl provider.
 * These are real functions (intl_stub.c), not macros, so code that takes their
 * address or redeclares them still compiles. Every lookup returns the message id:
 * the programs show their built-in (English) strings.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef GTK3WL_LIBINTL_H
#define GTK3WL_LIBINTL_H

#include <locale.h>

#ifndef LC_MESSAGES
#define LC_MESSAGES 6
#endif

#ifdef __cplusplus
extern "C" {
#endif

char *gettext(const char *msgid);
char *dgettext(const char *domainname, const char *msgid);
char *dcgettext(const char *domainname, const char *msgid, int category);
char *ngettext(const char *msgid1, const char *msgid2, unsigned long int n);
char *dngettext(const char *domainname, const char *msgid1, const char *msgid2, unsigned long int n);
char *dcngettext(const char *domainname, const char *msgid1, const char *msgid2, unsigned long int n, int category);
char *textdomain(const char *domainname);
char *bindtextdomain(const char *domainname, const char *dirname);
char *bind_textdomain_codeset(const char *domainname, const char *codeset);

#ifdef __cplusplus
}
#endif

#endif /* GTK3WL_LIBINTL_H */
