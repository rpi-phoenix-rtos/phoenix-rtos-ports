/* stk_phoenix_compat.h — force-included (-include) into every SuperTuxKart TU.
 *
 * Phoenix-RTOS's libc omits a few BSD socket constants that STK's bundled enet
 * (lib/enet/unix.c) and DNS resolver (lib/dnsc/dns.c) reference unconditionally.
 * They are plain integer constants, so defining fallbacks here — rather than
 * patching each bundled source — keeps the fix in one place and out of the
 * upstream trees. Macro-only: this header pulls in no system headers, so it is
 * safe to force-include ahead of both C and C++ translation units.
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef STK_PHOENIX_COMPAT_H
#define STK_PHOENIX_COMPAT_H

/* enet listen() backlog (lib/enet/unix.c). Linux uses 128. */
#ifndef SOMAXCONN
#define SOMAXCONN 128
#endif

/* recvmsg() flag reporting a truncated datagram (lib/enet/unix.c). Standard
 * Linux value; Phoenix's recvmsg ignores unknown flags. */
#ifndef MSG_TRUNC
#define MSG_TRUNC 0x20
#endif

/* one past the highest address family (lib/dnsc/dns.c bounds check). */
#ifndef AF_MAX
#define AF_MAX 42
#endif

#endif /* STK_PHOENIX_COMPAT_H */
