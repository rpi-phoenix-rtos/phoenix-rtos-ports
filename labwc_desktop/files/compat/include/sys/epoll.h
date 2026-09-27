/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: epoll_pwait() on top of the M6 epoll emulation (next on the
 * include path: weston-drm/compat). compat/src/lwphx_epoll_pwait.c.
 */
#include_next <sys/epoll.h>

#ifndef LWPHX_SYS_EPOLL_H
#define LWPHX_SYS_EPOLL_H

#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif
int epoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout, const sigset_t *sigmask);
#ifdef __cplusplus
}
#endif

#endif
