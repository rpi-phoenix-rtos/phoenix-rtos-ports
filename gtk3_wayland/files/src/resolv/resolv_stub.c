/*
 * gtk3-wayland: resolver stand-ins (see resolv.h). Phoenix-RTOS has no DNS record
 * resolver; GIO reports these queries as failed ("no such record" class errors).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <errno.h>
#include <resolv.h>

int res_init(void)
{
	return 0;
}

int res_query(const char *dname, int class_, int type, unsigned char *answer, int anslen)
{
	(void)dname;
	(void)class_;
	(void)type;
	(void)answer;
	(void)anslen;
	h_errno = NO_RECOVERY;
	return -1;
}

int dn_expand(const unsigned char *msg, const unsigned char *eom, const unsigned char *src, char *dst, int dstsiz)
{
	(void)msg;
	(void)eom;
	(void)src;
	(void)dst;
	(void)dstsiz;
	errno = EMSGSIZE;
	return -1;
}

int dn_skipname(const unsigned char *src, const unsigned char *eom)
{
	(void)src;
	(void)eom;
	errno = EMSGSIZE;
	return -1;
}
