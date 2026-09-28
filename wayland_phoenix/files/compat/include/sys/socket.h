/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm compat: libphoenix <sys/socket.h> has no AF_LOCAL/PF_LOCAL (the
 * POSIX-2008 synonyms of AF_UNIX that libwayland uses).
 */
#include_next <sys/socket.h>

#ifndef AF_LOCAL
#define AF_LOCAL AF_UNIX
#endif
#ifndef PF_LOCAL
#define PF_LOCAL AF_UNIX
#endif
