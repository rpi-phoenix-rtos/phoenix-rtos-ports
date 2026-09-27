/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm compat: libphoenix <fcntl.h> gaps.
 *  - O_NOFOLLOW (libseat's noop backend, the X-style lock files): 0 = plain open
 *  - file sealing (F_ADD_SEALS, F_SEAL_*): weston's os_create_anonymous_file()
 *    seals its memfd with F_SEAL_SHRINK and ignores the result; libphoenix's
 *    fcntl() answers an unknown command with EINVAL, which is that result
 */
#include_next <fcntl.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#define F_SEAL_SEAL   0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW   0x0004
#define F_SEAL_WRITE  0x0008
#endif
