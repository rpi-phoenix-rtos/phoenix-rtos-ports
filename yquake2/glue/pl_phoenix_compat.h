/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Phoenix Systems. Author: Witold Bołt.
 *
 * Force-included (-include) compat shim for the yQuake2 Phoenix port. Two
 * gaps between the Unix backend's assumptions and the Phoenix sysroot:
 *
 *  1. yQuake2's misc.c / system paths pull <unistd.h> only under per-OS
 *     #ifdef branches (none of which match Phoenix), leaving getcwd() &c.
 *     implicitly declared -> a hard error under GCC 14. Pull it globally so
 *     the real prototypes are in scope (no 64-bit pointer truncation).
 *
 *  2. Phoenix's <netinet/in.h> provides sockaddr_in6 / IN6_IS_ADDR_* /
 *     IPV6_* but not struct ipv6_mreq (used by the IPv6 multicast join in
 *     network.c). Supply it; the field names match the BSD/glibc layout.
 */
#ifndef PL_PHOENIX_COMPAT_H
#define PL_PHOENIX_COMPAT_H

#include <unistd.h>
#include <netinet/in.h>

struct ipv6_mreq {
	struct in6_addr ipv6mr_multiaddr;
	unsigned int    ipv6mr_interface;
};

/* network.c's NET_Sleep uses MAX(), which the Unix backend only gets from
 * <sys/param.h> on glibc (or its own __sun fallback). Phoenix's sys/param.h
 * doesn't provide it. */
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#endif /* PL_PHOENIX_COMPAT_H */
