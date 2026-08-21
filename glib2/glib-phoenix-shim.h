/*
 * Phoenix-RTOS compatibility shim for glib-2.56 (force-included via gcc -include).
 *
 * Fills libc gaps glib's source assumes but Phoenix libc lacks. glib's configure
 * already disables the langinfo/CODESET, dlopen, and NLS paths for Phoenix (no
 * langinfo.h, no dlfcn.h, --disable-nls), so this only needs to cover what still
 * leaks into compiled code.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#ifndef GLIB_PHOENIX_SHIM_H
#define GLIB_PHOENIX_SHIM_H

/* glib uses P_tmpdir for g_get_tmp_dir() fallback / mkstemp temp files. */
#ifndef P_tmpdir
#define P_tmpdir "/tmp"
#endif

/*
 * Phoenix <locale.h> defines LC_ALL..LC_TIME (0..5) but not LC_MESSAGES. glib's
 * ggettext.c calls setlocale(LC_MESSAGES, NULL). Define it as the next category
 * id; with --disable-nls setlocale just treats it as an unknown/no-op category.
 */
#include <locale.h>
#ifndef LC_MESSAGES
#define LC_MESSAGES 6
#endif

/*
 * NLS stubs. We build with --disable-nls so USE_NLS is off, but glib's
 * glibintl.h still references _(), and a few translation-unit-local uses of
 * gettext-family symbols can leak. With NLS disabled glib's own headers map
 * these to identity macros; this block is a belt-and-suspenders fallback in case
 * a TU pulls <libintl.h> (absent on Phoenix) directly. Define the macros so no
 * libintl symbols are needed at link time.
 */
#ifndef ENABLE_NLS
#ifndef gettext
#define gettext(Msgid) (Msgid)
#endif
#ifndef dgettext
#define dgettext(Domainname, Msgid) (Msgid)
#endif
#ifndef dcgettext
#define dcgettext(Domainname, Msgid, Category) (Msgid)
#endif
#ifndef ngettext
#define ngettext(Msgid1, Msgid2, N) ((N) == 1 ? (Msgid1) : (Msgid2))
#endif
#ifndef textdomain
#define textdomain(Domainname) ((char *)(Domainname))
#endif
#ifndef bindtextdomain
#define bindtextdomain(Domainname, Dirname) ((char *)(Dirname))
#endif
#ifndef bind_textdomain_codeset
#define bind_textdomain_codeset(Domainname, Codeset) ((char *)(Codeset))
#endif
#endif /* !ENABLE_NLS */

#endif /* GLIB_PHOENIX_SHIM_H */
