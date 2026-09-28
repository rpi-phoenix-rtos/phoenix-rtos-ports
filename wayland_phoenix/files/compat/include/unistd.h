/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm compat: libphoenix has no pipe2() (Weston's clipboard and client
 * launcher). compat/src/wlphx_misc.c implements it as pipe() + fcntl() -- not
 * atomic with respect to a concurrent fork()+exec() (the CLOEXEC race pipe2 closes).
 */
#include_next <unistd.h>

#ifndef WLPHX_UNISTD_H
#define WLPHX_UNISTD_H
#ifdef __cplusplus
extern "C"
#endif
int pipe2(int fds[2], int flags);
#endif
