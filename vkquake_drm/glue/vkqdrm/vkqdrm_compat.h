/*
 * vkquake-drm: force-included into every vkQuake TU (-include).
 *
 * mathlib.h switches on USE_SIMD/USE_NEON for aarch64 + __ARM_NEON but never includes the
 * intrinsics header; upstream gets it through its quakedef.h precompiled header, which this
 * build (one gcc per TU, like the port) does not use.
 *
 * libphoenix gap: <netinet/in.h> has struct in6_addr and IPV6_JOIN_GROUP but no struct
 * ipv6_mreq, which net_udp.c's IPv6 multicast path declares (single-player runs over the
 * loopback driver and never reaches it). Same bridge as the vkquake port's compat header.
 * libphoenix branch feat/ipv6mreq-execinfo (17c4fae) adds the struct together with
 * IPV6_ADD_MEMBERSHIP, so the bridge steps aside once the sysroot has it (a second
 * definition is an error under -std=gnu11); delete it after that merge.
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef VKQDRM_COMPAT_H
#define VKQDRM_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <netinet/in.h>
#if defined(__phoenix__) && !defined(IPV6_ADD_MEMBERSHIP)
struct ipv6_mreq {
	struct in6_addr ipv6mr_multiaddr;
	unsigned int ipv6mr_interface;
};
#endif

#endif
