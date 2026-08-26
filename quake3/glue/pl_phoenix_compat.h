/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Phoenix Systems. Author: Witold Bołt.
 *
 * Force-included (-include) compat shim for the quake3e Phoenix port.
 *
 * Sole job: defuse the msg_t type clash WITHOUT patching Q3 source.
 *
 * Phoenix's socket headers pull in a SysV-IPC message type:
 *   <netinet/in.h> -> <sys/sockport.h> -> <sys/msg.h> -> <phoenix/msg.h>
 * where <phoenix/msg.h> does `typedef struct _msg_t { ... } msg_t;`. That
 * collides head-on with Q3's network-buffer `typedef struct { ... } msg_t;`
 * (code/qcommon/qcommon.h) in every networking TU (net_ip.c, common.c, ...).
 *
 * A blanket -Dmsg_t=... cannot disambiguate: it renames BOTH typedefs to the
 * same name and just relocates the clash. Instead, because -include runs
 * before the TU's own text, we pre-parse the whole Phoenix socket/msg header
 * chain HERE with Phoenix's msg_t renamed to a private name, which trips every
 * include guard in that chain. When the TU later does #include <netinet/in.h>
 * the chain is a no-op, so Q3's msg_t is the only `msg_t` the TU ever sees.
 * Q3 never calls msgSend()/msgRecv(), so the renamed Phoenix prototypes have
 * no call sites and are harmless. Net result: zero Q3-source rename.
 */
#ifndef PL_PHOENIX_COMPAT_H
#define PL_PHOENIX_COMPAT_H

/* Rename Phoenix's IPC msg_t only while its header chain is parsed. */
#define msg_t phoenix_ipc_msg_t
#include <netinet/in.h>   /* sockport.h -> sys/msg.h -> phoenix/msg.h */
#include <sys/msg.h>      /* belt-and-braces: ensure the guard is set */
#undef msg_t

/* Phoenix's <netinet/in.h> provides sockaddr_in6 / IN6_* / IPV6_* but not
 * struct ipv6_mreq (used by net_ip.c's IPv6 multicast join). Field names
 * match the BSD/glibc layout. Same gap the yQuake2 port fills. */
#ifndef _PL_PHOENIX_IPV6_MREQ
#define _PL_PHOENIX_IPV6_MREQ
struct ipv6_mreq {
	struct in6_addr ipv6mr_multiaddr;
	unsigned int    ipv6mr_interface;
};
#endif

/* quake3e's aarch64 JIT (code/qcommon/vm_aarch64.c) flushes the instruction cache after emitting
 * code via __clear_cache(). The Phoenix toolchain exposes no prototype for the libgcc symbol, so
 * map it to the compiler builtin (which emits the aarch64 dc cvau / ic ivau / dsb / isb sequence
 * inline). Phoenix maps PROT_EXEC pages executable (kernel vm/map.c), so the JIT is viable. */
#ifndef __clear_cache
#define __clear_cache(beg, end) __builtin___clear_cache((void *)(beg), (void *)(end))
#endif

#endif /* PL_PHOENIX_COMPAT_H */
