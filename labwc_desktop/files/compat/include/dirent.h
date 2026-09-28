/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: dirfd() (fuzzel). libphoenix's DIR keeps a descriptor only
 * when it was made by fdopendir(); for opendir() streams dirfd() answers -1 with
 * ENOTSUP (compat/src/lwphx_misc.c), which fuzzel treats as "skip this directory".
 */
#include_next <dirent.h>

#ifndef LWPHX_DIRENT_H
#define LWPHX_DIRENT_H

#ifdef __cplusplus
extern "C" {
#endif
int dirfd(DIR *dirp);
#ifdef __cplusplus
}
#endif

#endif
