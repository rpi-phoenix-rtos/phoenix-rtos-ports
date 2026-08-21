/*
 * Phoenix-RTOS — stub libresolv: res_query/res_search/res_init that fail
 * cleanly. Exists only to satisfy glib-2.56's configure res_query() link probe;
 * gio's gresolver (the only consumer) is not built for libglib-2.0. Replace with
 * a real DNS resolver to enable name resolution on Phoenix.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#include "resolv.h"
#include <errno.h>

int res_query(const char *dname, int qclass, int type,
              unsigned char *answer, int anslen)
{
	(void)dname; (void)qclass; (void)type; (void)answer; (void)anslen;
	errno = ENOSYS;
	return -1;
}

int res_search(const char *dname, int qclass, int type,
               unsigned char *answer, int anslen)
{
	(void)dname; (void)qclass; (void)type; (void)answer; (void)anslen;
	errno = ENOSYS;
	return -1;
}

int res_init(void)
{
	return 0;
}
