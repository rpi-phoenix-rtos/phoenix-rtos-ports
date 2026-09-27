/*
 * gtk3-wayland: gettext without message catalogs (see libintl.h).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stddef.h>
#include <libintl.h>

static char current_domain[64] = "messages";

char *gettext(const char *msgid)
{
	return (char *)msgid;
}

char *dgettext(const char *domainname, const char *msgid)
{
	(void)domainname;
	return (char *)msgid;
}

char *dcgettext(const char *domainname, const char *msgid, int category)
{
	(void)domainname;
	(void)category;
	return (char *)msgid;
}

char *ngettext(const char *msgid1, const char *msgid2, unsigned long int n)
{
	return (char *)(n == 1 ? msgid1 : msgid2);
}

char *dngettext(const char *domainname, const char *msgid1, const char *msgid2, unsigned long int n)
{
	(void)domainname;
	return ngettext(msgid1, msgid2, n);
}

char *dcngettext(const char *domainname, const char *msgid1, const char *msgid2, unsigned long int n, int category)
{
	(void)domainname;
	(void)category;
	return ngettext(msgid1, msgid2, n);
}

char *textdomain(const char *domainname)
{
	unsigned int i;

	if (domainname != NULL) {
		for (i = 0; domainname[i] != '\0' && i + 1 < sizeof(current_domain); i++)
			current_domain[i] = domainname[i];
		current_domain[i] = '\0';
	}
	return current_domain;
}

char *bindtextdomain(const char *domainname, const char *dirname)
{
	(void)domainname;
	return (char *)dirname;
}

char *bind_textdomain_codeset(const char *domainname, const char *codeset)
{
	(void)domainname;
	return (char *)codeset;
}
