/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * Phoenix-RTOS platform backend for vkQuake (vkQuake is Copyright (C) id
 * Software, Inc. and the vkQuake developers, GPL-2.0-or-later). It implements
 * the vkQuake platform interface and is distributed under the same license as
 * the program it is built into; see COPYING in this directory.
 */
/*
 * vkq_phoenix_compat.h — force-included into every vkQuake engine TU.
 *
 * Bridges the few gaps between what vkQuake's portable engine assumes (glibc/SDL host)
 * and what the Phoenix aarch64 sysroot (libphoenix) + toolchain provide. Force-included
 * via -include so it lands before quakedef.h's own includes.
 *
 *   1. <arm_neon.h>: mathlib.h auto-#defines USE_SIMD/USE_NEON on aarch64+__ARM_NEON but
 *      never includes the intrinsics header (upstream relies on a PCH). r_world.c /
 *      r_brush.c then use float32x4_t / uint8x8_t. Pull it in here.
 *   2. <stddef.h>/<stdint.h>: miniz.h references size_t before q_stdinc.h establishes it
 *      in some include orders.
 *   3. libphoenix <math.h> gaps: it lacks copysign/copysignf, rint/rintf, remainder/
 *      remainderf, log2f, fmin/fmax (see the gap inventory). These are GCC builtins, so
 *      declaring the prototypes lets the engine compile; at LINK they resolve from libm /
 *      libgcc, OR — the upstreamable fix — by adding them to libphoenix's math (the same
 *      way the quakespasm/X11 ports filled libphoenix libc gaps). Declared here behind
 *      __PHOENIX_VKQ_MATH_GAPS so the proper libphoenix patch can drop this block later.
 */
#ifndef VKQ_PHOENIX_COMPAT_H
#define VKQ_PHOENIX_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

/* --- libphoenix <math.h> gap declarations (remove once libphoenix gains them) --- */
#define __PHOENIX_VKQ_MATH_GAPS 1
#ifdef __PHOENIX_VKQ_MATH_GAPS
double copysign(double x, double y);
float  copysignf(float x, float y);
double rint(double x);
float  rintf(float x);
double remainder(double x, double y);
float  remainderf(float x, float y);
float  log2f(float x);
double fmin(double x, double y);
double fmax(double x, double y);
float  fminf(float x, float y);
float  fmaxf(float x, float y);
#endif

/* --- libphoenix <netinet/in.h> gap: struct ipv6_mreq (net_udp.c IPv6 multicast) ---
 * libphoenix has struct in6_addr + IPV6_JOIN_GROUP/IPPROTO_IPV6 but not ipv6_mreq.
 * Only the IPv6 multiplayer path uses it; single-player runs over the loopback net
 * driver and never reaches it. Declared here so net_udp.c compiles; the upstreamable
 * fix is to add ipv6_mreq to libphoenix netinet/in.h. */
#include <netinet/in.h>
#ifndef __PHOENIX_HAVE_IPV6_MREQ
#define __PHOENIX_HAVE_IPV6_MREQ 1
struct ipv6_mreq {
	struct in6_addr ipv6mr_multiaddr;
	unsigned int    ipv6mr_interface;
};
#endif

#endif /* VKQ_PHOENIX_COMPAT_H */
