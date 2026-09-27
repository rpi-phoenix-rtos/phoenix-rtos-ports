/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm compat: libphoenix has no ppoll() (wl_display_poll in libwayland-
 * client). compat/src/wlphx_epoll.c provides it over poll(); a non-NULL sigmask
 * is applied around the poll() call, not atomically with it.
 */
#include_next <poll.h>

#ifndef WLPHX_POLL_H
#define WLPHX_POLL_H
#include <signal.h>
#include <time.h>
#ifdef __cplusplus
extern "C"
#endif
int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *tmo_p, const sigset_t *sigmask);
#endif
