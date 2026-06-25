/*
 * Phoenix-RTOS — fd_set compatibility shim for building xterm against the
 * ported Xlib (PREFIX/include/X11/Xpoll.h).
 *
 * Xpoll.h (and xterm's select()-based pty I/O loop) expect the glibc-style
 * fd_set internals:
 *   - a `fd_mask` integer type, and
 *   - a `__fds_bits` member on `struct fd_set` (Xpoll.h hardcodes
 *     `#define __X_FDS_BITS __fds_bits`).
 *
 * Phoenix's <sys/select.h> instead defines fd_set as
 *   struct fd_set_t_ { uint32_t fds_bits[(FD_SETSIZE+31)/32]; }
 * with NO `fd_mask` typedef and the member spelled `fds_bits` (no underscore).
 *
 * This shim is force-included (gcc -include) ahead of every translation unit
 * so that, before Xpoll.h is parsed:
 *   1. <sys/select.h> is pulled in (so struct fd_set_t_ is the real type), and
 *   2. `fd_mask` is typedef'd to the element type, and
 *   3. `__fds_bits` is aliased to the real member `fds_bits`.
 *
 * It is xterm-local (in CFLAGS only) and does NOT touch the shared X11 prefix,
 * so it cannot regress the other already-built X clients (twm/jwm/xeyes).
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#ifndef XTERM_PHOENIX_FDSET_SHIM_H
#define XTERM_PHOENIX_FDSET_SHIM_H

#include <stdint.h>
#include <sys/select.h>

/* Xpoll.h's NFDBITS uses sizeof(fd_mask); match the real fds_bits element. */
typedef uint32_t fd_mask;

/* Xpoll.h refers to the set's storage via the member name __fds_bits; Phoenix
 * names it fds_bits. Alias it so __XFDS_BITS(p,n) resolves. */
#ifndef __fds_bits
#define __fds_bits fds_bits
#endif

#endif /* XTERM_PHOENIX_FDSET_SHIM_H */
