/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: O_DIRECTORY (fuzzel opens its XDG directories with it for
 * openat()/fdopendir()). libphoenix has no such flag; open() of a directory
 * works without it, so it is 0 here -- the "must be a directory" check is lost,
 * and fdopendir() still refuses a non-directory (ENOTDIR).
 */
#include_next <fcntl.h>

#ifndef LWPHX_FCNTL_H
#define LWPHX_FCNTL_H

#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

#endif
